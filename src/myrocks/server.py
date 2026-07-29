"""mysqld lifecycle for the myrocks_optimizer experiment family.

One datadir per DB identity (engine x workload x index_layout x engine params);
init + load happen once per identity (D2 datadir caching), after which runs
only boot the server (seconds). Servers listen on a unix socket only.
"""

import hashlib
import os
import subprocess
import time

import mysql.connector

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DB_BASE = os.path.join(REPO_ROOT, "data", "sql", "db")


def build_dir(build_kind: str) -> str:
    return os.path.join(REPO_ROOT, "build-mysql", build_kind)


class MysqldServer:
    """Manages a single mysqld instance bound to one datadir."""

    def __init__(self, datadir: str, build_kind: str = "debug",
                 extra_args: list = None):
        self.datadir = os.path.abspath(datadir)
        self.build_kind = build_kind
        self.mysqld = os.path.join(build_dir(build_kind),
                                   "runtime_output_directory", "mysqld")
        # Unix socket paths are capped at ~107 chars by the kernel, so the
        # socket lives in /tmp keyed by datadir hash, not inside the datadir.
        dd_hash = hashlib.sha1(self.datadir.encode()).hexdigest()[:10]
        self.socket = f"/tmp/myrocks-bench-{dd_hash}.sock"
        self.errlog = os.path.join(self.datadir, "mysqld.err")
        self.extra_args = list(extra_args or [])
        self.proc = None

    # ---- identity / provenance -------------------------------------------

    def args(self) -> list:
        return [
            self.mysqld,
            "--no-defaults",
            f"--datadir={self.datadir}",
            f"--socket={self.socket}",
            f"--log-error={self.errlog}",
            "--skip-networking",
            "--local-infile=1",
        ] + self.extra_args

    def args_hash(self) -> str:
        """cnf-hash for per-run metadata (D1). Excludes machine-local paths."""
        stable = [a for a in self.args()[1:]
                  if not a.startswith(("--datadir", "--socket", "--log-error"))]
        return hashlib.sha256("\n".join(stable).encode()).hexdigest()[:12]

    def build_info(self) -> dict:
        info = {}
        path = os.path.join(build_dir(self.build_kind), "build-info.txt")
        with open(path) as f:
            for line in f:
                k, _, v = line.strip().partition("=")
                info[k] = v
        return info

    # ---- lifecycle --------------------------------------------------------

    def initialized(self) -> bool:
        return os.path.isdir(os.path.join(self.datadir, "mysql"))

    def initialize(self):
        """--initialize-insecure: root@localhost with empty password."""
        if self.initialized():
            return
        os.makedirs(self.datadir, exist_ok=True)
        subprocess.run(
            [self.mysqld, "--no-defaults", "--initialize-insecure",
             f"--datadir={self.datadir}", f"--log-error={self.errlog}"],
            check=True)

    def start(self, timeout: float = 120.0):
        assert self.proc is None, "server already started"
        self.initialize()
        self.proc = subprocess.Popen(
            self.args(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.time() + timeout
        last_err = None
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"mysqld exited rc={self.proc.returncode}; "
                    f"see {self.errlog}:\n{self._errlog_tail()}")
            try:
                self.connect().close()
                return
            except mysql.connector.Error as e:
                last_err = e
                time.sleep(0.3)
        raise RuntimeError(f"mysqld not ready in {timeout}s ({last_err}); "
                           f"see {self.errlog}:\n{self._errlog_tail()}")

    def connect(self, database: str = None) -> "mysql.connector.MySQLConnection":
        return mysql.connector.connect(
            unix_socket=self.socket, user="root", password="",
            database=database, autocommit=True, allow_local_infile=True)

    def stop(self, timeout: float = 600.0):
        if self.proc is None:
            return
        try:
            conn = self.connect()
            try:
                conn.cmd_shutdown()
            finally:
                conn.close()
        except mysql.connector.Error:
            self.proc.terminate()
        self.proc.wait(timeout=timeout)
        self.proc = None

    def _errlog_tail(self, lines: int = 15) -> str:
        try:
            with open(self.errlog) as f:
                return "".join(f.readlines()[-lines:])
        except OSError:
            return "(no error log)"

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
