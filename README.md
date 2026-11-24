# MicrosurgicalGuidanceFilterViewer

左右の録画済みカメラ映像を同時に再生し、それぞれに **Gaussian Blur + DynSWAVE** を適用して横に並べて表示する、独立した C++ / OpenCV の試作アプリです。

> このリポジトリは [DualIC4VarjoApp_AdaptiveGazeGuidance](https://github.com/Isasa2357/DualIC4VarjoApp_AdaptiveGazeGuidance) とは別です。元のVarjoアプリへの変更はありません。

## 機能

- 左右2本の動画（例: IC4カメラから保存した `*_left_raw.h264` / `*_right_raw.h264`）を入力
- それぞれの動画にフィルタを適用し、**左｜右** の順で横に連結して表示
- 画面の左右どちらにマウスを置いても、両眼に共通の正規化視線位置として扱う
- 円形ROI内は常に元映像を表示（ブラー・リングを除外）
- ROI外の Gaussian Blur: `sigma = (theta / pi) * 43.67`、最大強度を変えない `--blur-offset`、時間平滑化
- DynSWAVE: ROI方向へ収束する赤いリング、角度に基づく配置、最大3本程度、線幅は画像幅の5%、不透明度5〜50%、速度5〜30°/s（すべて調整可）
- 動画の再生FPSを指定し、処理が追いつかない場合は**左右同数のフレーム**をスキップ
- `--debug` で視線位置に目のマーカーと状態情報を表示
- 動画ファイルへの書き出しは行わない

## 要件

- Windows 10/11、Visual Studio 2022（C++17）
- CMake 3.21以上
- OpenCV 4.x（`core`, `imgproc`, `highgui`, `videoio`）
- `.h264` の再生には **FFmpeg対応でビルドされた OpenCV の Video I/O** が必要

## ビルド（PowerShell）

vcpkgでOpenCVをインストールしている場合:

```powershell
vcpkg install opencv4:x64-windows

cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
```

vcpkgのインストール場所によっては `VCPKG_ROOT` を設定してください。OpenCVを別の方法で導入している場合は、`OpenCV_DIR` を渡しても構いません。

## 実行例

```powershell
.\build\Release\MicrosurgicalGuidanceFilterViewer.exe `
  --left  "logs\experiment01\experiment01_left_raw.h264" `
  --right "logs\experiment01\experiment01_right_raw.h264" `
  --fps 160 `
  --roi 0.5 0.5 `
  --roi-radius 0.10 `
  --blur-offset 20 `
  --debug
```

`Q` または `Esc` で終了します。

```text
--left PATH                  左動画（必須）
--right PATH                 右動画（必須）
--fps N                      再生FPS（既定: 160）
--max-height N               左右それぞれの表示最大高さ（既定: 720）
--roi X Y                    ROI中心の正規化座標（既定: 0.5 0.5）
--roi-radius N               画像短辺に対するROI半径（既定: 0.10）
--plane-width-m N            各画像平面の幅[m]（既定: 1.0）
--blur-offset N              最大到達σに対する最小ブラー[%]（既定: 0）
--smoothing N                ブラー強度の時間平滑化時定数[s]（既定: 0.12）
--max-rings N                リング最大本数程度（既定: 3）
--ring-width N               画像幅に対するリング線幅[%]（既定: 5）
--min-opacity N              リング最小不透明度[%]（既定: 5）
--max-opacity N              リング最大不透明度[%]（既定: 50）
--min-speed N                リング最小角速度[deg/s]（既定: 5）
--speed-multiplier N         リング最大角速度の倍率（既定: 6）
--no-blur                    ブラー無効
--no-dynswave                DynSWAVE無効
--debug                      目のポインタと状態情報を表示
--help                       ヘルプ
```

## 挙動と制限

- **現段階では実際のアイトラッカーは使いません。** マウス位置を仮想的な視線として扱います。
- 視点はROI中心から画像平面に垂直に **1 m** 離れた場所とし、画像幅 `--plane-width-m` を使って画素距離を角度へ変換します。
- ROIの形状は円形です。左右の映像で同じ正規化ROI・視線座標を使います。
- DynSWAVEは3D球面テクスチャそのものではなく、球面上の角度半径を2D画像へ投影する**近似実装**です。速度・不透明度の線形変化は試作用の仕様です。
- 左右動画は**動画内でのフレーム順**で対応付けます。フレーム欠落や録画開始オフセットがある場合、`*_raw_pairs.csv` などによる厳密な再同期は今後の課題です。
- `raw.h264` は非圧縮RAWではなくH.264圧縮ビットストリームです。FPS情報が入っていない場合があるため、録画時の設定を `--fps` に指定してください。
- CPUで各フレームにGaussian Blurを適用するため、160 FPSを必ず達成できるわけではありません。負荷が高い場合は `--max-height` を下げてください。
- `--debug` では目のマーカーが描画上のROIにも重なることがあります。**フィルタ処理としてはROI画素を変更しません。**
- コードはVarjo本体やGPUのHLSL/D3D12パイプラインには接続していません。
