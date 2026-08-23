# Xray Windows release packaging

This fork ships Xray as a sibling executable next to `Telegram.exe`.
Put the matching Windows `xray.exe` into `out\Release` before packaging.

## Local package

After building `Telegram.exe` and `Updater.exe` in `out\Release`, run:

```powershell
Telegram\build\package_xray_win.ps1 -BuildTarget win64
```

The script writes artifacts to `out\Release\xray-package`:

- `replace-installed\` contains files that can replace an installed app
  folder manually.
- `tportable-xray-win64.<version>.zip` is the portable package.
- `tsetup-x64.xray-<version>.exe` is produced when Inno Setup is installed.

To update an existing portable copy, extract the new portable zip over the
old portable folder and keep the existing `TelegramForcePortable\tdata`
directory. The zip contains `TelegramForcePortable` so new unpacked copies
start in portable mode.

## Production package

The original `Telegram\build\build.bat` release pipeline now requires
`out\Release\xray.exe` and includes it in:

- the Inno Setup installer,
- the packed update file,
- the portable zip.

The production pipeline still requires Telegram Desktop's private signing and
release infrastructure.
