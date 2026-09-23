# qjsc-kmp

Prebuilt `qjsc-kmp`, the command line compiler of [quickjs-kmp](https://github.com/HarlonWang/quickjs-kmp): it turns a JavaScript module into bytecode that quickjs-kmp's `JsBytecode` loads. Bytecode is bound to the QuickJS commit it was compiled with, so use the `qjsc-kmp` version equal to the quickjs-kmp version your app depends on.

```sh
npx qjsc-kmp -m -n home --strip-source -o home.bin home.js
```

```js
import { binaryPath } from "qjsc-kmp";
binaryPath(); // absolute path of the binary for this machine
```

Binaries ship for macOS (arm64, x64) and Linux (x64, arm64; statically linked, any distribution) as the optional dependencies `@qjsc-kmp/<platform>`; npm installs only the one matching the machine. Elsewhere, build from source with `./gradlew :library:buildHostTools`.
