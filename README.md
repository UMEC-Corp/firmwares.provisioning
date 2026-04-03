# ESP-IDF Connectivity Component for UMEC Space IoT Cloud

ESP-IDF connectivity component for `UMEC Space IoT Cloud`.

Documentation:

- published docs site: https://umec-corp.github.io/firmwares.provisioning/
- open [`docs/index.html`](docs/index.html) in a browser
- open [`docs/reference.html`](docs/reference.html) for technical reference appendices
- open subsystem pages such as [`docs/provisioning.html`](docs/provisioning.html), [`docs/mqtt-cloud.html`](docs/mqtt-cloud.html), [`docs/storage-ota.html`](docs/storage-ota.html), and [`docs/field-runbooks.html`](docs/field-runbooks.html)
- GitHub Pages deploys automatically from the `docs/` folder through [`.github/workflows/pages.yml`](.github/workflows/pages.yml) on every push to `main`
- in the repository settings, set `Pages -> Build and deployment -> Source` to `GitHub Actions`

Build:

```powershell
tools\pio.cmd run -e core_esp32dev
```
