#include <opencv2/opencv.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSigmaMax = 43.67;
constexpr double kEyeDistanceMeters = 1.0;
constexpr char kWindowName[] = "Microsurgical Guidance Filter Viewer";

enum class FilterMode : int {
    None = 1,
    BlurAndDynSWAVE = 2,
    SingleEyeDarken = 3,
    SingleEyeBlur = 4,
    BlurWave = 5
};

enum class Eye { Left, Right };

const char* filterName(FilterMode mode) {
    switch (mode) {
        case FilterMode::None: return "None";
        case FilterMode::BlurAndDynSWAVE: return "Blur+DynSWAVE (both eyes)";
        case FilterMode::SingleEyeDarken: return "Darken (single eye)";
        case FilterMode::SingleEyeBlur: return "Blur (single eye)";
        case FilterMode::BlurWave: return "BlurWave (both eyes)";
    }
    return "Unknown";
}

const char* eyeName(Eye eye) {
    return eye == Eye::Left ? "left" : "right";
}

struct Options {
    std::string leftPath;
    std::string rightPath;

    double fps = 160.0;
    int maxHeight = 720;

    double roiX01 = 0.5;
    double roiY01 = 0.5;
    double roiRadiusShortAxis01 = 0.10;
    double planeWidthMeters = 1.0;

    double blurOffsetPercent = 0.0;
    double smoothingSeconds = 0.12;

    int maxRings = 3;
    double ringWidthPercent = 5.0;
    double minimumOpacity = 0.05;
    double maximumOpacity = 0.50;
    double minimumSpeedDps = 5.0;
    double speedMultiplier = 6.0;

    // ID 5: constant base blur; wave bands have stronger distance-dependent blur.
    double baseBlurSigma = 3.0;
    double waveBlurExtraMin = 2.0;
    double waveBlurExtraMax = 10.0;

    FilterMode initialFilterMode = FilterMode::None;
    Eye filterEye = Eye::Left;
    double darkenBrightness = 0.25;

    bool blurEnabled = true;
    bool dynswaveEnabled = true;
    bool debug = false;
};

struct MouseState {
    int eyeWidth = 1;
    int eyeHeight = 1;
    double x01 = 0.5;
    double y01 = 0.5;
};

struct FilterState {
    cv::Point roiCenter{};
    int roiRadiusPixels = 1;
    cv::Mat roiMask;

    double metersPerPixel = 1.0;
    double thetaMax = 0.0;
    double thetaRoi = 0.0;
    double maximumSigma = 0.0;

    double currentSigma = 0.0;
    double wavePhaseRadians = 0.0;
};

void printUsage() {
    std::cout
        << "Usage: MicrosurgicalGuidanceFilterViewer"
        << " --left LEFT.mp4 --right RIGHT.mp4 [options]\n\n"
        << "Filter IDs (keys 1-5 during playback):\n"
        << "  1  None (both eyes unchanged; default)\n"
        << "  2  Blur + DynSWAVE on both eyes (original behavior)\n"
        << "  3  Darken outside ROI in one eye\n"
        << "  4  Blur outside ROI in one eye\n"
        << "  5  Constant base Blur + moving stronger-blur waves (both eyes)\n\n"
        << "  --filter-id N            Initial filter ID, 1..5 (default: 1)\n"
        << "  --filter-eye left|right  Eye for filter 3/4 (default: left)\n"
        << "  --darken-brightness N    Outside ROI brightness % (default: 25)\n"
        << "  --fps N                  Playback FPS (default: 160)\n"
        << "  --max-height N           Maximum height per eye (default: 720)\n"
        << "  --roi X Y                Normalized ROI center (default: 0.5 0.5)\n"
        << "  --roi-radius N           ROI radius / image short axis (default: 0.10)\n"
        << "  --plane-width-m N        Physical width of each image in meters (default: 1)\n"
        << "  --blur-offset N          Minimum sigma as % of reachable maximum (default: 0)\n"
        << "  --smoothing N            Blur smoothing time constant in seconds (default: 0.12)\n"
        << "  --max-rings N            Maximum visible rings approximately (default: 3)\n"
        << "  --ring-width N           Ring line width as % of eye image width (default: 5)\n"
        << "  --min-opacity N          Minimum ring opacity in percent (default: 5)\n"
        << "  --max-opacity N          Maximum ring opacity in percent (default: 50)\n"
        << "  --min-speed N            Minimum ring speed in deg/s (default: 5)\n"
        << "  --speed-multiplier N     Maximum speed / minimum speed (default: 6)\n"
        << "  --base-blur-sigma N      ID 5 constant base sigma [px] (default: 3)\n"
        << "  --wave-blur-extra-min N  ID 5 min additional sigma [px] (default: 2)\n"
        << "  --wave-blur-extra-max N  ID 5 max additional sigma [px] (default: 10)\n"
        << "  --no-blur                Disable Gaussian Blur\n"
        << "  --no-dynswave            Disable DynSWAVE\n"
        << "  --debug                  Display eye markers and status information\n"
        << "  --help                   Show this help\n\n"
        << "Keys 1-5: switch filter. L / R: choose eye for filter 3/4.\n"
        << "Mouse: simulated gaze shared by both eyes. Q / Esc: quit.\n";
}

double parseReal(const std::string& value) {
    std::size_t consumed = 0;
    const double parsed = std::stod(value, &consumed);
    if (consumed != value.size() || !std::isfinite(parsed)) {
        throw std::invalid_argument("Invalid numeric value: " + value);
    }
    return parsed;
}

int parseInteger(const std::string& value) {
    std::size_t consumed = 0;
    const int parsed = std::stoi(value, &consumed);
    if (consumed != value.size()) {
        throw std::invalid_argument("Invalid integer value: " + value);
    }
    return parsed;
}

FilterMode parseFilterMode(int id) {
    if (id < 1 || id > 5) {
        throw std::invalid_argument("Filter ID must be in [1,5]");
    }
    return static_cast<FilterMode>(id);
}

Eye parseFilterEye(const std::string& value) {
    if (value == "left") return Eye::Left;
    if (value == "right") return Eye::Right;
    throw std::invalid_argument("--filter-eye must be left or right");
}

Options parseArguments(int argc, char** argv) {
    Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Missing argument for " + option);
            }
            return argv[++i];
        };

        if (option == "--left") options.leftPath = next();
        else if (option == "--right") options.rightPath = next();
        else if (option == "--filter-id") {
            options.initialFilterMode = parseFilterMode(parseInteger(next()));
        } else if (option == "--filter-eye") {
            options.filterEye = parseFilterEye(next());
        } else if (option == "--darken-brightness") {
            options.darkenBrightness = parseReal(next()) / 100.0;
        } else if (option == "--fps") options.fps = parseReal(next());
        else if (option == "--max-height") options.maxHeight = parseInteger(next());
        else if (option == "--roi") {
            options.roiX01 = parseReal(next());
            options.roiY01 = parseReal(next());
        } else if (option == "--roi-radius") {
            options.roiRadiusShortAxis01 = parseReal(next());
        } else if (option == "--plane-width-m") {
            options.planeWidthMeters = parseReal(next());
        } else if (option == "--blur-offset") {
            options.blurOffsetPercent = parseReal(next());
        } else if (option == "--smoothing") {
            options.smoothingSeconds = parseReal(next());
        } else if (option == "--max-rings") {
            options.maxRings = parseInteger(next());
        } else if (option == "--ring-width") {
            options.ringWidthPercent = parseReal(next());
        } else if (option == "--min-opacity") {
            options.minimumOpacity = parseReal(next()) / 100.0;
        } else if (option == "--max-opacity") {
            options.maximumOpacity = parseReal(next()) / 100.0;
        } else if (option == "--min-speed") {
            options.minimumSpeedDps = parseReal(next());
        } else if (option == "--speed-multiplier") {
            options.speedMultiplier = parseReal(next());
        } else if (option == "--base-blur-sigma") {
            options.baseBlurSigma = parseReal(next());
        } else if (option == "--wave-blur-extra-min") {
            options.waveBlurExtraMin = parseReal(next());
        } else if (option == "--wave-blur-extra-max") {
            options.waveBlurExtraMax = parseReal(next());
        } else if (option == "--no-blur") {
            options.blurEnabled = false;
        } else if (option == "--no-dynswave") {
            options.dynswaveEnabled = false;
        } else if (option == "--debug") {
            options.debug = true;
        } else if (option == "--help" || option == "-h") {
            printUsage();
            std::exit(0);
        } else {
            throw std::invalid_argument("Unknown option: " + option);
        }
    }

    if (options.leftPath.empty() || options.rightPath.empty()) {
        throw std::invalid_argument("--left and --right are required");
    }
    if (options.fps <= 0 || options.maxHeight <= 0) {
        throw std::invalid_argument("--fps and --max-height must be positive");
    }
    if (options.roiX01 < 0 || options.roiX01 > 1 ||
        options.roiY01 < 0 || options.roiY01 > 1) {
        throw std::invalid_argument("--roi must be within [0, 1]");
    }
    if (options.roiRadiusShortAxis01 <= 0 ||
        options.roiRadiusShortAxis01 > 1) {
        throw std::invalid_argument("--roi-radius must be within (0, 1]");
    }
    if (options.planeWidthMeters <= 0 || options.smoothingSeconds < 0) {
        throw std::invalid_argument("Invalid image width or smoothing");
    }
    if (options.blurOffsetPercent < 0 || options.blurOffsetPercent > 100) {
        throw std::invalid_argument("--blur-offset must be within [0, 100]");
    }
    if (options.maxRings < 1 ||
        options.ringWidthPercent <= 0 || options.ringWidthPercent > 100) {
        throw std::invalid_argument("Invalid DynSWAVE ring count or width");
    }
    if (options.minimumOpacity < 0 ||
        options.maximumOpacity > 1 ||
        options.minimumOpacity > options.maximumOpacity) {
        throw std::invalid_argument("Invalid DynSWAVE opacity range");
    }
    if (options.minimumSpeedDps < 0 || options.speedMultiplier < 1) {
        throw std::invalid_argument("Invalid DynSWAVE speed");
    }
    if (options.darkenBrightness < 0 || options.darkenBrightness > 1) {
        throw std::invalid_argument("--darken-brightness must be in [0,100]");
    }
    if (options.baseBlurSigma < 0.0 || options.baseBlurSigma > 100.0) {
        throw std::invalid_argument("--base-blur-sigma must be in [0,100]");
    }
    if (options.waveBlurExtraMin <= 0.0 ||
        options.waveBlurExtraMax < options.waveBlurExtraMin ||
        options.baseBlurSigma + options.waveBlurExtraMax > 100.0) {
        throw std::invalid_argument(
            "Wave blur requires 0 < extra-min <= extra-max and "
            "(base sigma + extra-max) <= 100 px"
        );
    }
    return options;
}

void openVideo(cv::VideoCapture& capture, const std::string& path) {
    if (!capture.open(path, cv::CAP_FFMPEG) &&
        !capture.open(path, cv::CAP_ANY)) {
        throw std::runtime_error(
            "Cannot open video: " + path +
            " (for raw H.264, OpenCV must support FFmpeg)"
        );
    }
}

void resizeFrame(cv::Mat& frame, int maximumHeight) {
    if (frame.rows <= maximumHeight) return;

    const double scale = static_cast<double>(maximumHeight) / frame.rows;
    const int newWidth = std::max(1, cvRound(frame.cols * scale));
    cv::resize(
        frame, frame,
        cv::Size(newWidth, maximumHeight),
        0.0, 0.0, cv::INTER_AREA
    );
}

void checkStereoFrames(const cv::Mat& left, const cv::Mat& right) {
    if (left.empty() || right.empty() ||
        left.size() != right.size() ||
        left.type() != CV_8UC3 || right.type() != CV_8UC3) {
        throw std::runtime_error(
            "Both videos must decode to equal-size 8-bit BGR frames"
        );
    }
}

void onMouse(int event, int x, int y, int, void* userdata) {
    if (event != cv::EVENT_MOUSEMOVE || !userdata) return;

    auto& mouse = *static_cast<MouseState*>(userdata);
    const int localX = (x >= mouse.eyeWidth) ? (x - mouse.eyeWidth) : x;
    const int px = std::clamp(localX, 0, mouse.eyeWidth - 1);
    const int py = std::clamp(y, 0, mouse.eyeHeight - 1);

    mouse.x01 = static_cast<double>(px) /
        std::max(1, mouse.eyeWidth - 1);
    mouse.y01 = static_cast<double>(py) /
        std::max(1, mouse.eyeHeight - 1);
}

FilterState initializeFilter(const cv::Size& size, const Options& options) {
    FilterState state;
    const int width = size.width;
    const int height = size.height;

    state.roiCenter = {
        std::clamp(cvRound(options.roiX01 * (width - 1)), 0, width - 1),
        std::clamp(cvRound(options.roiY01 * (height - 1)), 0, height - 1)
    };
    state.roiRadiusPixels = std::max(
        1, cvRound(options.roiRadiusShortAxis01 * std::min(width, height))
    );

    state.roiMask = cv::Mat::zeros(size, CV_8UC1);
    cv::circle(
        state.roiMask,
        state.roiCenter,
        state.roiRadiusPixels,
        cv::Scalar(255), cv::FILLED, cv::LINE_8
    );

    state.metersPerPixel = options.planeWidthMeters / width;
    double farthestDistancePixels = 0.0;
    for (const int x : {0, width - 1}) {
        for (const int y : {0, height - 1}) {
            farthestDistancePixels = std::max(
                farthestDistancePixels,
                std::hypot(
                    static_cast<double>(x - state.roiCenter.x),
                    static_cast<double>(y - state.roiCenter.y)
                )
            );
        }
    }

    state.thetaMax = std::atan2(
        farthestDistancePixels * state.metersPerPixel,
        kEyeDistanceMeters
    );
    state.thetaRoi = std::atan2(
        state.roiRadiusPixels * state.metersPerPixel,
        kEyeDistanceMeters
    );
    state.maximumSigma = kSigmaMax * state.thetaMax / kPi;
    state.currentSigma = state.maximumSigma *
        options.blurOffsetPercent / 100.0;

    return state;
}

struct FrameParameters {
    bool gazeInsideRoi = true;
    double gazeAngleRadians = 0.0;
    double gazeDistance01 = 0.0;
    double opacity = 0.0;
    double speedDegreesPerSecond = 0.0;
};

FrameParameters updateFilter(
    FilterState& state,
    const Options& options,
    const cv::Point& gaze,
    double elapsedSeconds
) {
    FrameParameters result;

    const double distancePixels = std::hypot(
        static_cast<double>(gaze.x - state.roiCenter.x),
        static_cast<double>(gaze.y - state.roiCenter.y)
    );
    result.gazeInsideRoi = distancePixels <= state.roiRadiusPixels;
    result.gazeAngleRadians = std::atan2(
        distancePixels * state.metersPerPixel,
        kEyeDistanceMeters
    );

    const double originalSigma = result.gazeInsideRoi
        ? 0.0
        : kSigmaMax * result.gazeAngleRadians / kPi;
    const double minimumSigma =
        state.maximumSigma * options.blurOffsetPercent / 100.0;

    const double normalizedSigma = state.maximumSigma > 0.0
        ? std::clamp(originalSigma / state.maximumSigma, 0.0, 1.0)
        : 0.0;
    const double targetSigma = minimumSigma +
        (state.maximumSigma - minimumSigma) * normalizedSigma;

    const double blend = options.smoothingSeconds > 0.0
        ? 1.0 - std::exp(-elapsedSeconds / options.smoothingSeconds)
        : 1.0;
    state.currentSigma += (targetSigma - state.currentSigma) * blend;

    if (!options.dynswaveEnabled || result.gazeInsideRoi) {
        state.wavePhaseRadians = 0.0;
        return result;
    }

    const double normalizedDistance = state.thetaMax > state.thetaRoi
        ? std::clamp(
            (result.gazeAngleRadians - state.thetaRoi) /
            (state.thetaMax - state.thetaRoi),
            0.0, 1.0
        )
        : 0.0;
    result.gazeDistance01 = normalizedDistance;

    result.opacity = options.minimumOpacity +
        (options.maximumOpacity - options.minimumOpacity) *
        normalizedDistance;

    result.speedDegreesPerSecond = options.minimumSpeedDps *
        (1.0 + (options.speedMultiplier - 1.0) * normalizedDistance);

    const double ringSpacing = state.thetaMax / options.maxRings;
    if (ringSpacing > 0.0) {
        state.wavePhaseRadians = std::fmod(
            state.wavePhaseRadians +
                result.speedDegreesPerSecond * (kPi / 180.0) *
                elapsedSeconds,
            ringSpacing
        );
    }
    return result;
}

cv::Mat renderEye(
    const cv::Mat& input,
    const FilterState& state,
    const Options& options,
    const FrameParameters& parameters,
    bool applyBlur,
    bool applyDynSWAVE
) {
    cv::Mat output;

    if (applyBlur && options.blurEnabled && state.currentSigma > 0.001) {
        const double sigma = state.currentSigma;
        const int kernel = 2 * static_cast<int>(std::ceil(2.0 * sigma)) + 1;
        cv::GaussianBlur(
            input, output,
            cv::Size(kernel, kernel),
            sigma, sigma,
            cv::BORDER_REFLECT_101
        );
    } else {
        output = input.clone();
    }

    // ROI always uses the unfiltered source image.
    input.copyTo(output, state.roiMask);

    if (!applyDynSWAVE || !options.dynswaveEnabled ||
        parameters.gazeInsideRoi ||
        parameters.opacity <= 0.0) {
        return output;
    }

    const double spacing = state.thetaMax / options.maxRings;
    if (spacing <= 0.0) return output;

    cv::Mat ringMask = cv::Mat::zeros(input.size(), CV_8UC1);
    const int ringWidthPixels = std::max(
        1, cvRound(input.cols * options.ringWidthPercent / 100.0)
    );

    // The rings move inward with increasing phase. Their radii are
    // angular distances from the ROI direction, projected onto the image.
    for (int i = 1; i <= options.maxRings; ++i) {
        const double angle = i * spacing - state.wavePhaseRadians;
        if (angle <= 0.0 || angle > state.thetaMax) continue;

        const int radiusPixels = cvRound(
            std::tan(angle) * kEyeDistanceMeters / state.metersPerPixel
        );
        cv::circle(
            ringMask,
            state.roiCenter,
            radiusPixels,
            cv::Scalar(255),
            ringWidthPixels,
            cv::LINE_AA
        );
    }

    ringMask.setTo(0, state.roiMask);

    // Overlay only ring pixels; anti-aliased mask values scale alpha.
    for (int y = 0; y < output.rows; ++y) {
        const auto* maskRow = ringMask.ptr<std::uint8_t>(y);
        auto* outputRow = output.ptr<cv::Vec3b>(y);

        for (int x = 0; x < output.cols; ++x) {
            const std::uint8_t coverage = maskRow[x];
            if (coverage == 0) continue;

            const float a = static_cast<float>(
                parameters.opacity * coverage / 255.0
            );
            auto& pixel = outputRow[x];
            pixel[0] = cv::saturate_cast<std::uint8_t>(
                pixel[0] * (1.0f - a)
            );
            pixel[1] = cv::saturate_cast<std::uint8_t>(
                pixel[1] * (1.0f - a)
            );
            pixel[2] = cv::saturate_cast<std::uint8_t>(
                pixel[2] * (1.0f - a) + 255.0f * a
            );
        }
    }

    return output;
}

// ID 5: apply a fixed base Gaussian blur, plus stronger blur within moving
// DynSWAVE bands. The band blur strength (but not the base) depends on
// gaze distance. Unlike ID 2, these bands contain no red overlay.
cv::Mat renderBlurWaveEye(
    const cv::Mat& input,
    const FilterState& state,
    const Options& options,
    const FrameParameters& parameters
) {
    if (!options.blurEnabled) return input.clone();

    cv::Mat output;
    if (options.baseBlurSigma > 0.0) {
        cv::GaussianBlur(
            input, output, cv::Size(0, 0),
            options.baseBlurSigma, options.baseBlurSigma,
            cv::BORDER_REFLECT_101
        );
    } else {
        output = input.clone();
    }

    // Only the ROI is excluded from the constant baseline blur.
    input.copyTo(output, state.roiMask);

    // The wave vanishes when looking inside the ROI. Base blur remains.
    if (!options.dynswaveEnabled || parameters.gazeInsideRoi) {
        return output;
    }

    const double spacing = state.thetaMax / options.maxRings;
    if (spacing <= 0.0) return output;

    // Annular wave masks share the geometry and inward motion of ID 2.
    cv::Mat bandMask = cv::Mat::zeros(input.size(), CV_8UC1);
    const int bandWidthPixels = std::max(
        1, cvRound(input.cols * options.ringWidthPercent / 100.0)
    );

    for (int i = 1; i <= options.maxRings; ++i) {
        const double angle = i * spacing - state.wavePhaseRadians;
        if (angle <= 0.0 || angle > state.thetaMax) continue;

        const int radiusPixels = cvRound(
            std::tan(angle) * kEyeDistanceMeters / state.metersPerPixel
        );
        cv::circle(
            bandMask, state.roiCenter, radiusPixels,
            cv::Scalar(255), bandWidthPixels, cv::LINE_AA
        );
    }

    // Never apply a wave across the original-image ROI.
    bandMask.setTo(0, state.roiMask);
    if (cv::countNonZero(bandMask) == 0) return output;

    const double additionalSigma =
        options.waveBlurExtraMin +
        (options.waveBlurExtraMax - options.waveBlurExtraMin) *
        parameters.gazeDistance01;
    const double waveSigma = options.baseBlurSigma + additionalSigma;

    cv::Mat strongerBlur;
    cv::GaussianBlur(
        input, strongerBlur, cv::Size(0, 0),
        waveSigma, waveSigma, cv::BORDER_REFLECT_101
    );

    // Blend the stronger blur only in the bands. Use the antialiased
    // coverage as alpha to avoid a hard edge at the annulus boundaries.
    for (int y = 0; y < output.rows; ++y) {
        const auto* mask = bandMask.ptr<std::uint8_t>(y);
        const auto* strong = strongerBlur.ptr<cv::Vec3b>(y);
        auto* base = output.ptr<cv::Vec3b>(y);
        for (int x = 0; x < output.cols; ++x) {
            if (mask[x] == 0) continue;
            const float alpha = static_cast<float>(mask[x]) / 255.0f;
            for (int channel = 0; channel < 3; ++channel) {
                base[x][channel] = cv::saturate_cast<std::uint8_t>(
                    (1.0f - alpha) * base[x][channel] +
                    alpha * strong[x][channel]
                );
            }
        }
    }

    return output;
}

// Only outside-ROI pixels are darkened, keeping the ROI unchanged.
cv::Mat renderDarkenedEye(
    const cv::Mat& input, const FilterState& state, double brightness
) {
    cv::Mat output;
    input.convertTo(output, -1, brightness);
    input.copyTo(output, state.roiMask);
    return output;
}

void drawEyeMarker(cv::Mat& image, const cv::Point& gaze) {
    cv::ellipse(
        image, gaze, cv::Size(16, 9), 0, 0, 360,
        cv::Scalar(0, 0, 0), 5, cv::LINE_AA
    );
    cv::ellipse(
        image, gaze, cv::Size(16, 9), 0, 0, 360,
        cv::Scalar(255, 255, 255), 2, cv::LINE_AA
    );
    cv::circle(
        image, gaze, 6, cv::Scalar(0, 0, 255),
        cv::FILLED, cv::LINE_AA
    );
    cv::circle(
        image, gaze, 3, cv::Scalar(0, 0, 0),
        cv::FILLED, cv::LINE_AA
    );
}

cv::Mat makeDisplayFrame(
    const cv::Mat& processedLeft,
    const cv::Mat& processedRight,
    const Options& options,
    const FilterState& filter,
    const FrameParameters& parameters,
    FilterMode activeFilter,
    Eye filterEye,
    const cv::Point& gaze
) {
    cv::Mat left = processedLeft;
    cv::Mat right = processedRight;

    if (options.debug) {
        left = left.clone();
        right = right.clone();
        drawEyeMarker(left, gaze);
        drawEyeMarker(right, gaze);
    }

    cv::Mat combined;
    cv::hconcat(left, right, combined);
    if (!options.debug) return combined;

    // Put debug status outside the image so it does not overwrite the ROI.
    cv::Mat statusBar(36, combined.cols, CV_8UC3, cv::Scalar(25, 25, 25));
    std::ostringstream status;
    status << std::fixed << std::setprecision(2)
           << "filter=" << static_cast<int>(activeFilter)
           << " (" << filterName(activeFilter) << ")";
    if (activeFilter == FilterMode::BlurWave) {
        const double waveSigma =
            options.baseBlurSigma + options.waveBlurExtraMin +
            (options.waveBlurExtraMax - options.waveBlurExtraMin) *
            parameters.gazeDistance01;
        status << " baseSigma=" << options.baseBlurSigma
               << " waveSigma=" << waveSigma;
    } else {
        status << " eye=" << eyeName(filterEye)
               << " sigma=" << filter.currentSigma << " px";
    }
    status << " angle=" << (parameters.gazeAngleRadians * 180.0 / kPi)
           << " deg speed=" << parameters.speedDegreesPerSecond
           << " deg/s opacity=" << (parameters.opacity * 100.0) << "%";

    cv::putText(
        statusBar, status.str(), cv::Point(8, 24),
        cv::FONT_HERSHEY_SIMPLEX, 0.55,
        cv::Scalar(255, 255, 255), 1, cv::LINE_AA
    );

    cv::Mat display;
    cv::vconcat(combined, statusBar, display);
    return display;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseArguments(argc, argv);

        cv::VideoCapture leftCapture;
        cv::VideoCapture rightCapture;
        openVideo(leftCapture, options.leftPath);
        openVideo(rightCapture, options.rightPath);

        cv::Mat leftFrame;
        cv::Mat rightFrame;
        if (!leftCapture.read(leftFrame) || !rightCapture.read(rightFrame)) {
            throw std::runtime_error("No decodable stereo frame found");
        }
        resizeFrame(leftFrame, options.maxHeight);
        resizeFrame(rightFrame, options.maxHeight);
        checkStereoFrames(leftFrame, rightFrame);

        FilterState filter = initializeFilter(leftFrame.size(), options);
        FilterMode activeFilter = options.initialFilterMode;
        Eye filterEye = options.filterEye;

        auto logFilter = [&]() {
            std::cout << "Filter ID " << static_cast<int>(activeFilter)
                      << " = " << filterName(activeFilter)
                      << " ; single-eye target: " << eyeName(filterEye)
                      << '\n';
        };

        MouseState mouse;
        mouse.eyeWidth = leftFrame.cols;
        mouse.eyeHeight = leftFrame.rows;
        mouse.x01 = options.roiX01;
        mouse.y01 = options.roiY01;

        std::cout << "Left: " << options.leftPath << '\n'
                  << "Right: " << options.rightPath << '\n'
                  << "Playback FPS: " << options.fps << '\n'
                  << "Output per eye: " << leftFrame.cols << "x"
                  << leftFrame.rows << '\n'
                  << "Reachable max sigma: " << filter.maximumSigma << " px\n"
                  << "Mouse: simulated gaze (shared by both eyes)\n"
                  << "Keys: 1-5 = filter, L/R = eye, Q/Esc = exit\n";
        logFilter();

        cv::namedWindow(kWindowName, cv::WINDOW_AUTOSIZE);
        cv::setMouseCallback(kWindowName, onMouse, &mouse);

        using Clock = std::chrono::steady_clock;
        const auto playbackStart = Clock::now();
        auto previousUpdate = playbackStart;
        std::uint64_t frameIndex = 0;

        for (;;) {
            const auto now = Clock::now();
            const double dt = std::max(
                0.0,
                std::chrono::duration<double>(now - previousUpdate).count()
            );
            previousUpdate = now;

            const cv::Point gaze(
                cvRound(mouse.x01 * (leftFrame.cols - 1)),
                cvRound(mouse.y01 * (leftFrame.rows - 1))
            );

            const FrameParameters parameters = updateFilter(
                filter, options, gaze, dt
            );

            // ID 2: old bilateral guidance. IDs 3/4: one eye only.
            cv::Mat processedLeft = leftFrame;
            cv::Mat processedRight = rightFrame;
            switch (activeFilter) {
                case FilterMode::None:
                    break;
                case FilterMode::BlurAndDynSWAVE:
                    processedLeft = renderEye(
                        leftFrame, filter, options, parameters, true, true
                    );
                    processedRight = renderEye(
                        rightFrame, filter, options, parameters, true, true
                    );
                    break;
                case FilterMode::SingleEyeDarken:
                    if (filterEye == Eye::Left) {
                        processedLeft = renderDarkenedEye(
                            leftFrame, filter, options.darkenBrightness
                        );
                    } else {
                        processedRight = renderDarkenedEye(
                            rightFrame, filter, options.darkenBrightness
                        );
                    }
                    break;
                case FilterMode::SingleEyeBlur:
                    if (filterEye == Eye::Left) {
                        processedLeft = renderEye(
                            leftFrame, filter, options, parameters, true, false
                        );
                    } else {
                        processedRight = renderEye(
                            rightFrame, filter, options, parameters, true, false
                        );
                    }
                    break;
                case FilterMode::BlurWave:
                    processedLeft = renderBlurWaveEye(
                        leftFrame, filter, options, parameters
                    );
                    processedRight = renderBlurWaveEye(
                        rightFrame, filter, options, parameters
                    );
                    break;
            }

            cv::imshow(
                kWindowName,
                makeDisplayFrame(
                    processedLeft, processedRight,
                    options, filter, parameters,
                    activeFilter, filterEye, gaze
                )
            );

            const int key = cv::waitKey(1) & 0xff;
            if (key == 27 || key == 'q' || key == 'Q') break;
            if (key >= '1' && key <= '5') {
                const auto requestedMode = static_cast<FilterMode>(key - '0');
                if (requestedMode != activeFilter) {
                    activeFilter = requestedMode;
                    filter.wavePhaseRadians = 0.0;
                    logFilter();
                }
            } else if (key == 'l' || key == 'L') {
                if (filterEye != Eye::Left) {
                    filterEye = Eye::Left;
                    logFilter();
                }
            } else if (key == 'r' || key == 'R') {
                if (filterEye != Eye::Right) {
                    filterEye = Eye::Right;
                    logFilter();
                }
            }
            if (cv::getWindowProperty(
                    kWindowName, cv::WND_PROP_VISIBLE) < 1.0) {
                break;
            }

            // Keep playback aligned with the wall clock.
            // Skip the same number of frames on both sources when behind.
            const std::uint64_t nextIndex = frameIndex + 1;
            const auto deadline = playbackStart +
                std::chrono::duration_cast<Clock::duration>(
                    std::chrono::duration<double>(
                        static_cast<double>(nextIndex) / options.fps
                    )
                );
            std::this_thread::sleep_until(deadline);

            const double elapsedSeconds =
                std::chrono::duration<double>(
                    Clock::now() - playbackStart
                ).count();
            const auto clockIndex = static_cast<std::uint64_t>(
                std::floor(elapsedSeconds * options.fps)
            );
            const std::uint64_t targetIndex =
                std::max(nextIndex, clockIndex);

            bool finished = false;
            for (std::uint64_t skipped = nextIndex;
                 skipped < targetIndex; ++skipped) {
                if (!leftCapture.grab() || !rightCapture.grab()) {
                    finished = true;
                    break;
                }
            }
            if (finished) break;

            if (!leftCapture.read(leftFrame) ||
                !rightCapture.read(rightFrame)) {
                break;
            }
            resizeFrame(leftFrame, options.maxHeight);
            resizeFrame(rightFrame, options.maxHeight);
            checkStereoFrames(leftFrame, rightFrame);
            if (leftFrame.size() != filter.roiMask.size()) {
                throw std::runtime_error(
                    "Frame resolution changed during playback"
                );
            }
            frameIndex = targetIndex;
        }

        cv::destroyAllWindows();
        std::cout << "Playback ended.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
