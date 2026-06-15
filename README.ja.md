[English](README.md) | **日本語**

<img width="1920" height="1080" alt="Alpha Recorder Hero" src="https://github.com/user-attachments/assets/0d18331a-3550-4cf7-a78a-a810488a6b42" />

# :movie_camera: Alpha Recorder for OBS

Alpha
Recorderは、OBSでの録画時にアルファを保持した状態で録画できるプラグインです。メインの動画とは別にグレイスケールのマスクとしてアルファを書き出すため、特殊な色空間やビデオコーデックを使用する必要はありません。

難しい操作は一切不要で、いつも通りOBSで録画するだけで使えます。

1. OBSを起動する
2. OBSの「録画開始」をクリック
3. お好きなタイミングで録画を停止
4. 録画した動画と同じ場所に `.alpha.mov` または `.alpha.mp4`
   というファイルができています

> [!IMPORTANT]
> Alpha
> Recorderは通常の録画ファイルにアルファを追加するものではありません。メインの動画は引き続きRGBとして残り、別のファイルとしてマスク動画が書き出されます。適宜、動画編集ソフトなどで合成してご利用ください。

## 機能

- HEVCハードウェアエンコーダー（NVENC /
  AMF）を使用したアルファマスクの書き出しに対応
- PNG MOV形式でのロスレスアルファマスク書き出しにも対応
- エンコーダーの設定を簡単に切り替え可能
- アルファとメイン動画の開始位置とフレームが完全に同期
- Windows、macOS、およびLinux版OBSで動作

## クイックスタート

1. [Releases](https://github.com/adzukyat/obs-alpha-recorder-plugin/releases)
   から最新版をダウンロードして解凍します。
2. OBSが起動している場合はいったん終了してください。
3. 解凍したフォルダからプラグインをOBSのプラグインディレクトリにコピーします。ディレクトリは以下の通りです。

| プラットフォーム | コピー元                | コピー先                                         |
| ---------------- | ----------------------- | ------------------------------------------------ |
| Windows          | `alpha_recorder.dll`    | `C:\Program Files\obs-studio\obs-plugins\64bit\` |
| macOS            | `alpha_recorder.plugin` | `/Applications/OBS.app/Contents/PlugIns/`        |
| Linux            | `alpha_recorder.so`     | `/usr/lib/obs-plugins/`                          |

もしOBSをインストールした場所が上記と異なる場合は、OBSのインストールディレクトリを確認して適切な場所にコピーしてください。Linuxの場合はディレクトリがディストリビューションやインストール方法によって異なることがありますので、OBSのドキュメントなどで確認してください。

4. OBSを起動して、OBSの「ツール」メニューに「Alpha Recorder
   Settings」が追加されていることを確認します。
5. 設定で「Enabled」にチェックが入っていることを確認して「OK」をクリックします。

基本的にはこれで準備完了です。あとはいつも通りOBSで録画するだけで、アルファマスクが別のファイルとして書き出されます。必要に応じて書き出し形式の設定を変更してください。

## 設定

<img width="540" height="581" src="https://github.com/user-attachments/assets/ecfaecf7-79db-4c6d-8e28-62fc2aa7f350" />

※HEVC関連の設定はNVENCやAMFが利用できない環境では表示されません。

## トラブルシューティング

| 症状                               | 考えられる理由                                                      |
| ---------------------------------- | ------------------------------------------------------------------- |
| HEVCオプションが表示されない       | NVENCやAMFエンコーダーが使用できない環境で実行している              |
| マスクが出力されない               | Alpha Recorderの設定の「Enabled」がオフになっている                 |
| Spout2でアルファが黒くなってしまう | Spout2のソース設定で「Composite Mode」をOpaqueからDefaultに変更する |

## ビルド

Alpha
RecorderはC++で書かれたOBSプラグインで、サブモジュールにOBSを含みます。ビルドにはCMakeとOBSのビルド環境が必要です。また、アプリケーションE2Eテストを実行するにはBunランタイムが必要になります。

### 1. サブモジュールの初期化

```sh
git submodule update --init --recursive
```

### 2. OBSのブートストラップ

```sh
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
```

### 3. 構成とビルド

Windows:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

macOS:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64-relwithdebinfo
```

Linux:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
```

以降、`--preset`
オプションの値やディレクトリは環境に合わせて読み替えてください。

- `windows-x64-msvc-relwithdebinfo`
- `macos-arm64-relwithdebinfo`
- `linux-x64-relwithdebinfo`

### 4. OBSツリーのステージング

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_stage_obs_tree
```

ステージングされたランタイムは `out/stage/obs` に書き出されます。

> [!TIP]
> LinuxはOBSのビルド依存関係、およびFFmpegのツール類が必要になります。例えばUbuntu
> 24.04の場合は以下のコマンドでインストールできます。

```sh
sudo apt update && \
sudo apt install -y build-essential cmake ninja-build git pkg-config curl unzip zip \
  extra-cmake-modules ffmpeg libavcodec-dev libavformat-dev libavutil-dev \
  libavfilter-dev libavdevice-dev libswscale-dev libjansson-dev uthash-dev \
  qt6-base-dev qt6-base-private-dev qt6-svg-dev libx11-dev libx11-xcb-dev libxcomposite-dev libxdamage-dev \
  libxrandr-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-dev \
  libxcb-render0-dev libxcb-shape0-dev libxcb-xfixes0-dev libxcb-shm0-dev \
  libxcb-composite0-dev libxcb-randr0-dev libxcb-xinerama0-dev libxcb-xinput-dev \
  libgl1-mesa-dev libegl1-mesa-dev libwayland-dev wayland-protocols \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libv4l-dev libudev-dev libdrm-dev libva-dev libvpl-dev \
  libcurl4-openssl-dev libmbedtls-dev libspeexdsp-dev libasio-dev libsimde-dev \
  libwebsocketpp-dev libx264-dev libluajit-5.1-dev swig python3-dev nlohmann-json3-dev \
  libqrcodegencpp-dev libpci-dev && \
curl -fsSL https://bun.sh/install | bash
```

## テスト

単体テストの実行

```sh
ctest --test-dir out/build/windows-x64-msvc -C RelWithDebInfo -L unit --output-on-failure
```

Deterministic E2Eテストの実行

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_e2e
```

アプリケーションE2Eテストの実行

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

アプリケーションE2Eテストでは実際にOBSが起動し、録画からフレーム同期検証までの一連の流れがWebSocket経由で自動で実行されます。全ランタイムでの検証が走りますが、非対応環境はスキップされます。

> [!WARNING]
> AMFでの動作は私がRadeon
> GPUを持っていないためテストしていません。バグを見つけた場合はパッチの提出をお願いします。
