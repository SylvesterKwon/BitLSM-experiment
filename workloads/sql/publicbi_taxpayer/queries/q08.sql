SELECT nppes_provider_last_org_name AS nppes_provider_last_org_name FROM taxpayer WHERE ((nppes_provider_first_name = 'JOHN') AND (nppes_provider_state = 'WA')) GROUP BY nppes_provider_last_org_name;
