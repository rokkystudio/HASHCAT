# hashcat-v7.1.2-android-arm64

Prebuilt hashcat runtime package for Android `arm64-v8a`.

## Asset

- `hashcat-v7.1.2-android-arm64.zip`
- SHA-256: `697f3452b642926a402d0e83e1b891fda5ceb3b7b4362d44339ee9d46d513538`

## Payload

- hashcat executable: 1098448 bytes
- modules: 598
- bridges: 4
- feeds: 6
- OpenCL files: 1886
- rules: 96
- tunings: 14
- pcfg files: 2
- hashcat.hcstat2: 240526 bytes

## Source

- upstream: `hashcat/hashcat`
- branch: `master`
- commit: `1e86dd8c81ed2073791da9ddc0b6181dc0a3b832`
- wrapper version tag: `v7.1.2`

Current upstream status at packaging time:

```text
## master...origin/master [behind 2]
```

## Notes

This is not an APK. It is a native hashcat runtime bundle intended for Android apps that unpack it into app-private storage and execute the `hashcat` binary as a native process.

Only `arm64-v8a` is included in this release.