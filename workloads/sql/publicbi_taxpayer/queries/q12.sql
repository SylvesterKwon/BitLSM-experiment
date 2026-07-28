SELECT nppes_provider_city AS nppes_provider_city FROM taxpayer WHERE ((hcpcs_description = 'Initial hospital care') AND (nppes_provider_state = 'WA')) GROUP BY nppes_provider_city;
