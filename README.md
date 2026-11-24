# MicrosurgicalGuidanceFilterViewer

左右の録画済みカメラ映像を同時に再生し、**Blur + DynSWAVE／片眼暗化／片眼Blur／無変調**を切り替えて横に並べて表示する、独立した C++ / OpenCV の試作アプリです。

> このリポジトリは [DualIC4VarjoApp_AdaptiveGazeGuidance](https://github.com/Isasa2357/DualIC4VarjoApp_AdaptiveGazeGuidance) とは別です。元のVarjoアプリへの変更はありません。

## 機能

- 左右2本の動画（例: IC4カメラから保存した `*_left_raw.h264` / `*_right_raw.h264`）を入力
- **左｜右** の順で横に連結して表示し、数字キー `1`〜`4` でフィルタを切り替え
- 初期表示は**左右とも無変調（ID 1）**。起動引数 `--filter-id` で初期フィルタを指定可能
- ID 2では左右とも従来のBlur + DynSWAVE、ID 3/4では**片眼のみ**を加工し反対眼は元映像のまま
- ID 3/4の対象眼は `--filter-eye left|right`（既定は左）または実行中の `L` / `R` キーで選択
- ROI外暗化は `--darken-brightness` で強度指定（既定は元画像の25%の明るさ）
- 画面の左右どちらにマウスを置いても、両眼に共通の正規化視線位置として扱う
- 円形ROI内は常に元映像を表示（ブラー・リング・暗化を除外）
- ROI外の Gaussian Blur: `sigma = (theta / pi) * 43.67`、最大強度を変えない `--blur-offset`、時間平滑化
- DynSWAVE: ROI方向へ収束する赤いリング、角度に基づく配置、最大3本程度、線幅は画像幅の5%、不透明度5〜50%、速度5〜30°/s（すべて調整可）
- 動画の再生FPSを指定し、処理が追いつかない場合は**左右同数のフレーム**をスキップ
- `--debug` で視線位置に目のマーカーと状態情報を表示
- 動画ファイルへの書き出しは行わない

## フィルタの割り当て

| 数字キー / フィルタID | 左眼（対象眼が既定のleftの場合） | 右眼 |
|---|---|---|
| `1`（デフォルト） | 無変調 | 無変調 |
| `2` | Blur + DynSWAVE | Blur + DynSWAVE |
| `3` | ROI外暗化 | 無変調 |
| `4` | ROI外Blur | 無変調 |

ID 3/4の場合、**どちらか一方の眼にのみ**処理を適用し、もう一方の眼は無変調で表示します。`--filter-eye right` または実行中に `R` キーを押した場合は、左右の適用関係が逆になります。

- `L` キー: ID 3/4で処理する眼を左に変更
- `R` キー: ID 3/4で処理する眼を右に変更
- `1`〜`4` キー: フィルタIDを切り替え
- マウス移動: 両眼共通の疑似視線位置を変更
- `Q` / `Esc` キー: 終了

初期IDと対象眼は以下のように指定できます。指定しなければ **ID 1（無変調）・対象眼は左**です。

```powershell
.\build-vs2026-explicit\Release\MicrosurgicalGuidanceFilterViewer.exe `
  --left "video\left.mp4" `
  --right "video\right.mp4" `
  --fps 160 `
  --filter-id 3 `
  --filter-eye right `
  --darken-brightness 25
```

`--filter-id 2` は従来どおり**両眼に同じBlur + DynSWAVE**を適用します。起動後は数字キーだけで切り替えられます。

## 要件

- Windows 10/11、**Visual Studio 2026（C++デスクトップ開発ワークロード）**
- **CMake 4.2以上**（Visual Studio 2026ジェネレーター対応版）
- Git とインターネット接続（初回のOpenCV取得用）
- **vcpkgやOpenCVの事前インストールは不要**

CMakeの`FetchContent`で **OpenCV 4.14.0** をGitHubからダウンロードし、必要なモジュール（`core`, `imgproc`, `highgui`, `videoio` とその依存モジュール）をソースから静的ビルドします。OpenCVの構成時にWindows用FFmpegバックエンドも取得します。

## ビルド（PowerShell / Visual Studio 2026）

リポジトリのルートで、追加した `CMakePresets.json` を使って実行します。

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
```

直接ジェネレーター名を指定する場合は、次の2コマンドでも同じ構成になります。

```powershell
cmake -S . -B build-vs2026 -G "Visual Studio 18 2026" -A x64
cmake --build build-vs2026 --config Release --parallel 4
```

**Visual Studio 2026のCMakeジェネレーターは `Visual Studio 18 2026` です。** `Visual Studio 19 2026` は存在しません。確認コマンドは `cmake --help` と `cmake --version` です。

Visual Studio 2026がインストールされていてもCMakeがインスタンスを自動検出できない場合は、次のようにインストール先を明示します。

```powershell
cmake -S . -B build-vs2026-explicit -G "Visual Studio 18 2026" -A x64 `
  "-DCMAKE_GENERATOR_INSTANCE=C:/Program Files/Microsoft Visual Studio/18/Community"

cmake --build build-vs2026-explicit --config Release --parallel 16
```

### OpenCVのヘッダーエラーについて

`opencv2/opencv_modules.hpp` はOpenCVのCMake構成時に**ビルドディレクトリ側へ生成されるヘッダー**です。このプロジェクトの `CMakeLists.txt` は、OpenCVのソース側と生成ヘッダー側の両方をincludeパスに指定します。以前の構成から更新した場合は `git pull` した後に上の `cmake -S . -B ...` を再実行し、設定を再生成してください。OpenCVのライブラリを最初からビルドし直す必要は通常ありません。

### MSVCリンクエラー（LNK2038）について

OpenCVを静的ビルドした場合、OpenCV側は静的MSVCランタイムの `/MT`（Debugでは `/MTd`）を使用します。アプリケーションが標準設定の `/MD` のままだと、リンク時に `RuntimeLibrary: MT_StaticRelease != MD_DynamicRelease` という `LNK2038` エラーが発生します。

現在の `CMakeLists.txt` ではアプリケーション側も同じランタイムへ統一しています。アップデート後は**同じビルドディレクトリを維持したまま**CMakeの構成とビルドを再実行してください。通常、OpenCVのソースを再取得したり、ライブラリを再ビルドしたりする必要はありません。

実行ファイルは `build-vs2026-explicit/Release/MicrosurgicalGuidanceFilterViewer.exe`（別のビルドフォルダの場合も `<ビルドフォルダ>/Release/`）へ出力するよう明示しています。FFmpeg用DLLも同じフォルダにコピーします。

初回はOpenCVの取得とコンパイルが必要です。2回目以降は同じ `build-vs2026/` の取得済みソース・ビルド結果を再利用します。

以前に `build/` や `build-fetch/` で別のVisual Studioジェネレーターを使用した場合、CMakeキャッシュと競合することがあります。上記の `build-vs2026/` は独立した新規ビルドディレクトリです。

```powershell
.\build-vs2026\Release\MicrosurgicalGuidanceFilterViewer.exe --help
```

### FFmpegとH.264入力について

- `.h264` のデコードに必要なFFmpeg用DLLはOpenCVの構成時に別途ダウンロードされます。
- CMakeではDLLの存在を検査し、ダウンロードできなかった場合はエラーとして通知します。
- Releaseビルド後、`build-vs2026/Release/`にアプリ本体と`opencv_videoio_ffmpeg4140_64.dll`が並ぶ構成です。
- DLLを別の場所へ移動する場合は、実行ファイルと一緒に配置してください。
- ダウンロードがネットワーク環境で遮断される場合、OpenCVやFFmpegの取得に失敗することがあります。その場合はCMakeの構成ログを確認してください。



## 実行例

```powershell
.\build-vs2026\Release\MicrosurgicalGuidanceFilterViewer.exe `
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
--filter-id N                初期フィルタID（1〜4、既定: 1）
--filter-eye left|right      ID 3/4の対象眼（既定: left）
--darken-brightness N        ROI外の明るさ[%]（既定: 25）
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
--no-blur                    ブラー無効（ID 2/4にも適用）
--no-dynswave                DynSWAVE無効（ID 2にも適用）
--debug                      目のポインタと状態情報を表示
--help                       ヘルプ
```

## 挙動と制限

- 数字キー `1`〜`4` が全体の表示モードを決めます。ID 3/4では選択した眼のみ変調し、もう一方は無変調です。
- **現段階では実際のアイトラッカーは使いません。** マウス位置を仮想的な視線として扱います。
- 視点はROI中心から画像平面に垂直に **1 m** 離れた場所とし、画像幅 `--plane-width-m` を使って画素距離を角度へ変換します。
- ROIの形状は円形です。左右の映像で同じ正規化ROI・視線座標を使います。
- DynSWAVEは3D球面テクスチャそのものではなく、球面上の角度半径を2D画像へ投影する**近似実装**です。速度・不透明度の線形変化は試作用の仕様です。
- 左右動画は**動画内でのフレーム順**で対応付けます。フレーム欠落や録画開始オフセットがある場合、`*_raw_pairs.csv` などによる厳密な再同期は今後の課題です。
- `raw.h264` は非圧縮RAWではなくH.264圧縮ビットストリームです。FPS情報が入っていない場合があるため、録画時の設定を `--fps` に指定してください。
- CPUで各フレームにGaussian Blurを適用するため、160 FPSを必ず達成できるわけではありません。負荷が高い場合は `--max-height` を下げてください。
- `--debug` では目のマーカーが描画上のROIにも重なることがあります。**フィルタ処理としてはROI画素を変更しません。**
- コードはVarjo本体やGPUのHLSL/D3D12パイプラインには接続していません。
