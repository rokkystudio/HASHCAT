# hashcat-v7.1.2-android-arm64

Prebuilt hashcat runtime package for Android `arm64-v8a`.

## Asset

- `hashcat-v7.1.2-android-arm64.zip`
- SHA-256: `238c3faa62aa528995504e26aca862a33f4b4ddf850bd29af8bf90090dff53b1`

## Payload

- hashcat executable: 1099136 bytes
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
- commit: `a36879049b91175768a9fff1407c0042ce3e7503`
- wrapper version tag: `v7.1.2`

Current upstream status at packaging time:

```text
## master...origin/master
```

## Notes

This is not an APK. It is a native hashcat runtime bundle intended for Android apps that unpack it into app-private storage and execute the `hashcat` binary as a native process.

Only `arm64-v8a` is included in this release.