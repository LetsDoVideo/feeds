# Feeds — Maintenance Notes

## libcurl Dependency Management

Feeds ships a proxy `libcurl.dll` that sits between OBS and the Zoom SDK, allowing both to use their own incompatible libcurl builds without conflict. Three libcurl files live in `zoom-sdk/bin/`:

| File | What it is |
|------|------------|
| `libcurl.dll` | Our proxy (built from `proxy/libcurl_proxy.cpp`) |
| `libcurl_obs.dll` | OBS's original libcurl |
| `libcurl_zoom.dll` | Zoom SDK's libcurl |

### If OBS updates their libcurl

1. Download a fresh OBS install
2. Copy `bin/64bit/libcurl.dll` from the OBS install folder
3. Rename it to `libcurl_obs.dll`
4. Replace `zoom-sdk/bin/libcurl_obs.dll` in this repo with the new file
5. Commit and rebuild

### If Zoom SDK updates their libcurl

1. Copy `libcurl.dll` from the new Zoom SDK's `bin/` folder
2. Rename it to `libcurl_zoom.dll`
3. Replace `zoom-sdk/bin/libcurl_zoom.dll` in this repo with the new file
4. Check if the new SDK added any new libcurl exports:
   - Use a tool like `dumpbin /exports libcurl_zoom.dll` to list exports
   - Compare against the current `proxy/libcurl_zoom.def`
   - If there are new exports, add them to `proxy/libcurl_zoom.def` and add matching stubs to `proxy/libcurl_proxy.cpp`
5. Commit and rebuild

### If both update at the same time

Do both procedures above before rebuilding.

---

## Auth Helper (FeedsLogin.exe)

`FeedsLogin.exe` is built from `helper/FeedsAuthHelper.cpp`. It receives the `ldvfeeds://` deeplink from the browser after Zoom OAuth and passes the auth code to the plugin via named pipe `\\.\pipe\FeedsAuth`.

The Windows protocol handler for `ldvfeeds://` is registered in the Windows Registry at runtime by the plugin itself (`obs_module_load`). No installer changes needed if the helper is updated.

---

## Zoom SDK Updates

When updating to a new Zoom Meeting SDK version:

1. Replace all files in `zoom-sdk/bin/` with the new SDK's bin files (keeping `libcurl_obs.dll` — that's ours, not Zoom's)
2. Replace all files in `zoom-sdk/h/` with the new SDK's headers
3. Replace `zoom-sdk/lib/sdk.lib` with the new SDK's lib file
4. Check the SDK release notes for any API changes that affect the plugin
5. Rebuild and test

---

## Azure Code Signing

Signing uses Azure Trusted Signing under account `feeds-signing`. The three GitHub secrets required are stored in the `release` environment in the GitHub repo settings:

- `AZURE_CLIENT_ID`
- `AZURE_TENANT_ID`  
- `AZURE_SUBSCRIPTION_ID`

If signing stops working, check that the federated credential for the repo is still active in the Azure portal.
