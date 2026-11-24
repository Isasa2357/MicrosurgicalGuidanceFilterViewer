# MicrosurgicalGuidanceFilterViewer

左右の録画済みカメラ映像を同時に再生し、**Blur + DynSWAVE／Blur波版DynSWAVE／片眼暗化／片眼Blur／無変調**を切り替えて横に並べて表示する、独立した C++ / OpenCV の試作アプリです。

> このリポジトリは [DualIC4VarjoApp_AdaptiveGazeGuidance](https://github.com/Isasa2357/DualIC4VarjoApp_AdaptiveGazeGuidance) とは別です。元のVarjoアプリへの変更はありません。

## 機能

- 左右2本の動画（例: IC4カメラから保存した `*_left_raw.h264` / `*_right_raw.h264`）を入力
- **左｜右** の順で横に連結して表示し、数字キー `1`〜`5` でフィルタを切り替え
- 初期表示は**左右とも無変調（ID 1）**。起動引数 `--filter-id` で初期フィルタを指定可能
- ID 2では左右とも従来の赤いBlur + DynSWAVE、ID 3/4では**片眼のみ**を加工し反対眼は元映像のまま
- **ID 5**では左右とも、一定強度のベースBlurに、視線距離に応じて強度と速度が増加する移動Blur波を重ねます（赤いリングなし）
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
| `5` | 固定ベースBlur + 強い移動Blur波 | 固定ベースBlur + 強い移動Blur波 |

ID 3/4の場合、**どちらか一方の眼にのみ**処理を適用し、もう一方の眼は無変調で表示します。`--filter-eye right` または実行中に `R` キーを押した場合は、左右の適用関係が逆になります。

- `L` キー: ID 3/4で処理する眼を左に変更
- `R` キー: ID 3/4で処理する眼を右に変更
- `1`〜`5` キー: フィルタIDを切り替え
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

`--filter-id 2` は従来どおり**両眼に同じBlur + 赤いDynSWAVE**を適用します。ID 5では赤い波を使わず、移動する強いBlurの帯を使います。起動後は数字キーで切り替えられます。

### ID 5：固定ベースBlur + Blur波DynSWAVE

ID 5は**左右両眼**に適用し、赤いリングは描画しません。まずROI外全域に固定σのGaussian Blurを適用し、その上から移動するリング状の帯だけ、より強いGaussian Blur画像を合成します。波の幅を元の2倍に拡張し、元の波幅を中央の強いBlur領域として維持します。拡張した内側・外側それぞれ元幅の半分を遷移領域に割り当て、smoothstep曲線でBase Blurから波Blurへ滑らかに合成します。ROI内は拡張・遷移領域も含めて除外し、元画像を保持します。

- ベースのBlur強度は視線距離に依存せず一定です（デフォルトσ=3 px）。
- 波のBlurはROIからの視線距離が大きいほど強くなります（デフォルトσ=5〜13 px）。
- 波の移動速度も視線距離に応じて5〜30°/sの範囲で増加します。
- 波は既存DynSWAVEと同じ中心・本数・方向でROI方向に移動します。強いBlurの中央部分は元の線幅を維持し、遷移領域を含む合計の表示幅は2倍です。
- 視線がROI内にある場合は波だけが消え、ベースBlurは維持されます。
- ROI内部の画像は常に無変調です。

ROI外の正規化視線距離を u（0〜1）とすると、デフォルトでは以下の式になります。

```text
σ_base = 3 px
σ_wave = σ_base + 2 + 8u px  （5〜13 px）
speed = 5 × (1 + 5u) °/s     （5〜30 °/s）
```

| ID 5の専用引数 | 内容 | デフォルト |
|---|---|---|
| `--base-blur-sigma` | 固定ベースBlurのσ [px] | 3 |
| `--wave-blur-extra-min` | 波の追加σの最小値 [px] | 2 |
| `--wave-blur-extra-max` | 波の追加σの最大値 [px] | 10 |

`--ring-width` は**強いBlurが100%適用される中央領域の幅**を画像幅の百分率で指定します（既定5%）。遷移領域を含む実際の波幅はその2倍（既定10%）で、中央から内側・外側へそれぞれ元幅の半分を追加します。

波の線幅と本数は `--ring-width` と `--max-rings`、速度は `--min-speed` と `--speed-multiplier` で変更します。ID 5では `--blur-offset` と不透明度オプションは使いません。`--no-dynswave` で波のみを無効化でき、`--no-blur` でID 5のBlurをすべて無効化します。

実行例：既存の動画指定に `--filter-id 5 --base-blur-sigma 3 --wave-blur-extra-min 2 --wave-blur-extra-max 10` を追加してください。

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
--filter-id N                初期フィルタID（1〜5、既定: 1）
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
--base-blur-sigma N          ID 5の固定ベースσ [px]（既定: 3）
--wave-blur-extra-min N      ID 5の波の追加σの最小値 [px]（既定: 2）
--wave-blur-extra-max N      ID 5の波の追加σの最大値 [px]（既定: 10）
--no-blur                    ブラー無効（ID 2/4にも適用）
--no-dynswave                DynSWAVE無効（ID 2にも適用）
--debug                      目のポインタと状態情報を表示
--help                       ヘルプ
```

## 挙動と制限

- 数字キー `1`〜`5` が全体の表示モードを決めます。ID 3/4では選択した眼のみ変調し、もう一方は無変調です。
- **現段階では実際のアイトラッカーは使いません。** マウス位置を仮想的な視線として扱います。
- 視点はROI中心から画像平面に垂直に **1 m** 離れた場所とし、画像幅 `--plane-width-m` を使って画素距離を角度へ変換します。
- ROIの形状は円形です。左右の映像で同じ正規化ROI・視線座標を使います。
- DynSWAVEは3D球面テクスチャそのものではなく、球面上の角度半径を2D画像へ投影する**近似実装**です。速度・不透明度の線形変化は試作用の仕様です。
- 左右動画は**動画内でのフレーム順**で対応付けます。フレーム欠落や録画開始オフセットがある場合、`*_raw_pairs.csv` などによる厳密な再同期は今後の課題です。
- `raw.h264` は非圧縮RAWではなくH.264圧縮ビットストリームです。FPS情報が入っていない場合があるため、録画時の設定を `--fps` に指定してください。
- CPUで各フレームにGaussian Blurを適用するため、160 FPSを必ず達成できるわけではありません。負荷が高い場合は `--max-height` を下げてください。
- `--debug` では目のマーカーが描画上のROIにも重なることがあります。**フィルタ処理としてはROI画素を変更しません。**
- コードはVarjo本体やGPUのHLSL/D3D12パイプラインには接続していません。
