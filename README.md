# MicrosurgicalGuidanceFilterViewer (Direct3D 12)

左右の録画動画を再生し、視線誘導フィルタを**Direct3D 12 (D3D12) Compute Shader**で処理するWindows専用C++アプリです。画面表示もWin32/DXGIスワップチェーンへ変更しました。D3D12Helper、CD3DX12、DirectXTKは使用していません。

元のDualIC4VarjoApp_AdaptiveGazeGuidanceリポジトリは変更していません。

## 処理構成

~~~text
左右のMP4/H.264
   ↓ OpenCV + FFmpeg: CPUで動画デコード、任意の縮小、BGR→RGBA変換
CPUフレーム
   ↓ D3D12 Upload Heap / CopyTextureRegion
GPUテクスチャ（左右を横並び）
   ↓ D3D12 Compute Shader
      ├ 水平/垂直Gaussian Blur
      ├ 赤いDynSWAVE
      ├ ROI保護・片眼暗化・片眼Blur
      └ 固定Base Blur + 距離依存Blur波
   ↓ D3D12 Graphics Pipeline
DXGI Swap Chain → Win32ウィンドウ
~~~

Gaussian Blur・波状マスク・画像合成・画面表示はGPUで行い、処理後の映像はCPUへ読み戻しません。**動画のデコード・リサイズ・色変換は引き続きOpenCV（CPU）**です。GPU動画デコードとゼロコピー転送は実装していません。

## フィルタID

| 数字キー / ID | 左眼 | 右眼 |
|---|---|---|
| 1（起動時のデフォルト） | 無変調 | 無変調 |
| 2 | 距離依存Blur + 赤色DynSWAVE | 同左 |
| 3 | ROI外を暗化 | 無変調 |
| 4 | ROI外をBlur | 無変調 |
| 5 | 固定Base Blur + 強い移動Blur波 | 同左 |

ID 3/4で変調する眼はデフォルトで左眼です。L/Rキーまたは --filter-eye left|right で変更すると、反対眼は無変調になります。ID 2と5は常に両眼を変調します。ROI内はすべてのモードで元画像を表示します。

### ID 5のBlur波

- ベースのGaussian Blurは**視線距離によらず一定σ=3 px**（デフォルト）
- 波のBlurは距離に応じて**σ=5〜13 px**（デフォルト）
- 波の移動速度は距離に応じて**5〜30°/s**（デフォルト）
- 赤色リングを描画せず、帯の内部のBlur強度だけを上げます
- 中央に元の波幅を維持し、内側/外側へそれぞれ元幅の半分を追加してsmoothstepで滑らかに合成します（波全体の幅は元の2倍）
- 視線がROI内にある場合は波が消え、ベースBlurのみになります。ROI内部の画素は常に無変調です

ROI外の正規化視線距離をu（0〜1）とすると、既定値では以下の関係になります。

~~~text
sigma_base = 3 px                    （固定）
sigma_wave = 3 + 2 + 8u px          （5〜13 px）
wave_speed = 5 × (1 + 5u) deg/s     （5〜30 deg/s）
~~~

--ring-width 5 の場合、中央の強いBlurは画像幅の5%、両側の遷移領域を合わせた波全体は画像幅の10%です。

### キーボードとマウス

- 数字キー 1〜5：フィルタ切り替え
- L / R：ID 3・4の対象眼を変更
- マウス移動：両眼共通の疑似視線（どちらの眼の映像上からでも操作可能）
- Q / Esc：終了
- ウィンドウサイズ変更：縦横比を維持して表示

--debug を付けると、GPU上で視線を示す目のマーカーを描画し、ウィンドウタイトルへフィルタ状態を表示します。

## 環境とビルド

必要環境はWindows 10/11、D3D12対応GPU、Visual Studio 2026のC++デスクトップ開発とWindows SDK、CMake 4.2以上、Gitです。**vcpkgは不要**です。

CMake FetchContentがOpenCV 4.14.0（core/imgproc/videoioと依存ライブラリ）を取得し、H.264/MP4用のFFmpeg DLLもOpenCVが取得します。D3D12、DXGI、D3DCompileのライブラリはWindows SDK付属品です。

~~~powershell
git pull origin main
cmake --preset vs2026
cmake --build --preset vs2026-release
~~~

このプリセットは使用中のVisual Studioインストール先 C:/Program Files/Microsoft Visual Studio/18/Community と、既存の build-vs2026-explicit ディレクトリを指定します。

手動で指定する場合は以下でもビルドできます。

~~~powershell
cmake -S . -B build-vs2026-explicit -G "Visual Studio 18 2026" -A x64 "-DCMAKE_GENERATOR_INSTANCE=C:/Program Files/Microsoft Visual Studio/18/Community"
cmake --build build-vs2026-explicit --config Release --parallel 16
~~~

出力ファイルの構成は以下のとおりです。

~~~text
build-vs2026-explicit/
└── Release/
    ├── MicrosurgicalGuidanceFilterViewer.exe
    ├── opencv_videoio_ffmpeg4140_64.dll
    └── shaders/
        ├── blur.hlsl
        ├── composite.hlsl
        └── present.hlsl
~~~

HLSLはアプリ起動時にD3DCompileFromFileでコンパイルするため、**shadersフォルダを実行ファイルと一緒に配置**する必要があります。CMakeで自動コピーします。

## 実行例

~~~powershell
.\build-vs2026-explicit\Release\MicrosurgicalGuidanceFilterViewer.exe --left ".\video\05_left2right_leftcam_1.mp4" --right ".\video\05_left2right_rightcam_1.mp4" --fps 160 --roi 0.5 0.5 --filter-id 5 --base-blur-sigma 3 --wave-blur-extra-min 2 --wave-blur-extra-max 10 --debug
~~~

引数 --filter-id を指定しない場合はID 1（無変調）で起動します。--fpsには実際の動画FPSを指定してください。

### 主要コマンドライン引数

| 引数 | 内容 | デフォルト |
|---|---|---|
| --left / --right | 左右の動画ファイル（必須） | — |
| --filter-id | 起動時フィルタID 1〜5 | 1 |
| --filter-eye | ID 3/4の対象眼 left/right | left |
| --fps | 再生FPS | 160 |
| --max-height | 各眼の最大映像高さ | 720 |
| --roi X Y | ROI中心の正規化位置 | 0.5 0.5 |
| --roi-radius | ROI半径/画像短辺 | 0.10 |
| --darken-brightness | ID 3のROI外明るさ[%] | 25 |
| --blur-offset | ID 2/4の最小Blur[%] | 0 |
| --smoothing | ID 2/4の時間平滑化[秒] | 0.12 |
| --max-rings | 波の本数（1〜8） | 3 |
| --ring-width | 波の中心幅[%] | 5 |
| --min-opacity / --max-opacity | ID 2の赤い波の不透明度[%] | 5 / 50 |
| --min-speed | 最小波速度[°/s] | 5 |
| --speed-multiplier | 最大波速度の倍率 | 6 |
| --base-blur-sigma | ID 5の固定Base σ[px] | 3 |
| --wave-blur-extra-min | ID 5の追加波Blur最小σ[px] | 2 |
| --wave-blur-extra-max | ID 5の追加波Blur最大σ[px] | 10 |
| --no-blur | Blurを無効化 | 無効 |
| --no-dynswave | 波を無効化 | 無効 |
| --debug | GPU視線マーカーと状態を表示 | 無効 |

引数の全一覧は実行ファイルの --help でも確認できます。

## ファイル構成

- main.cpp：CLI、左右動画再生、視線・ROI・波角度・フィルタID管理
- src/D3D12StereoRenderer.hpp：GPUフレーム引数とレンダラー公開API
- src/D3D12StereoRenderer.cpp：Win32、D3D12デバイス、GPUアップロードと同期、DXGI描画
- shaders/blur.hlsl：各眼の境界を越えない分離Gaussian Blur
- shaders/composite.hlsl：フィルタID 1〜5のGPU合成とROI保護
- shaders/present.hlsl：左右ステレオ表示・デバッグ視線マーカー
- CMakeLists.txt / CMakePresets.json：OpenCV自動取得、Windows SDKとのリンク、実行構成

## 制限事項

- CPU/OpenCVで動画をデコードし、D3D12のUpload Heapを介してGPUへ送ります。完全なGPUデコードは未実装です。
- 左右は動画のフレーム順で対応付けます。録画メタデータによる厳密な同期補正は未実装です。
- Gaussian Blurは2パスの分離フィルタです。ID 5はBaseとWaveの最大4パスを実行します。
- GPU完了を毎フレーム待つ単純な方式です。160 FPSなどの高フレームレート達成は保証しません。
- **このD3D12版はGitHub上でコードを確認した段階で、Windowsでの実ビルドと表示動作は未検証です。**
