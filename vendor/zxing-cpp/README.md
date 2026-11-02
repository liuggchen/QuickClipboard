# ZXing-C++

- 上游：https://github.com/zxing-cpp/zxing-cpp
- 版本：v3.1.1
- 来源：https://github.com/zxing-cpp/zxing-cpp/archive/refs/tags/v3.1.1.zip
- 下载压缩包 SHA-256：`9d9884d560baf745f4930307adc07555104ea96f2c66b42bae754de799649980`

保留上游 `core` 目录和 `LICENSE`，源码未修改。根目录 CMake 只启用二维码读写，设置 `ZXING_WRITERS=OLD` 使用自带编码器，关闭其他码制并静态链接；不引入 libzint，构建无需联网。

主要源码遵循 Apache-2.0；字符集转换使用随附的 libzueci（BSD-3-Clause）。随程序分发的许可文本见仓库根目录 `THIRD_PARTY_NOTICES.txt`。
