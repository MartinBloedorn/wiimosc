#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace {

const double sPi = 3.14159265358979323846;
const int sZeroCrossings = 8;       //!< per side of the sinc kernel
const int sKernelResolution = 256; //!< kernel table entries per input sample

double sinc(double x)
{
    return x == 0.0 ? 1.0 : std::sin(sPi * x) / (sPi * x);
}

/*! \brief Blackman window over -1..1. */
double blackman(double t)
{
    return 0.42 + 0.5 * std::cos(sPi * t) + 0.08 * std::cos(2.0 * sPi * t);
}

} // namespace

std::vector<float> resample(const std::vector<float> &input, int inputRate, int outputRate)
{
    if (inputRate == outputRate || input.empty())
        return input;

    // Cutoff a bit below the lower Nyquist frequency, in cycles per input sample.
    const double cutoff = 0.45 * std::min(inputRate, outputRate) / inputRate;
    const int halfWidth = static_cast<int>(std::ceil(sZeroCrossings / (2.0 * cutoff)));
    const double step = static_cast<double>(inputRate) / outputRate;
    const int inputSize = static_cast<int>(input.size());

    // The kernel is symmetric; tabulate one side, finely enough that linear interpolation is exact enough.
    std::vector<float> kernel(static_cast<size_t>(halfWidth * sKernelResolution + 2), 0.0f);
    for (size_t i = 0; i + 1 < kernel.size(); ++i) {
        const double distance = static_cast<double>(i) / sKernelResolution;
        kernel[i] = static_cast<float>(2.0 * cutoff * sinc(2.0 * cutoff * distance) * blackman(distance / halfWidth));
    }

    std::vector<float> output(static_cast<size_t>(input.size() / step));
    for (size_t n = 0; n < output.size(); ++n) {
        const double center = n * step;
        const int first = std::max(0, static_cast<int>(std::floor(center)) - halfWidth + 1);
        const int last = std::min(inputSize - 1, static_cast<int>(std::floor(center)) + halfWidth);
        double sum = 0.0;
        for (int k = first; k <= last; ++k) {
            const double position = std::abs(center - k) * sKernelResolution;
            const size_t index = static_cast<size_t>(position);
            const double fraction = position - index;
            sum += input[k] * (kernel[index] + fraction * (kernel[index + 1] - kernel[index]));
        }
        output[n] = static_cast<float>(sum);
    }
    return output;
}

void normalize(std::vector<float> *samples, float peak)
{
    float maximum = 0.0f;
    for (float sample : *samples)
        maximum = std::max(maximum, std::abs(sample));
    if (maximum <= 0.0f)
        return;
    for (float &sample : *samples)
        sample *= peak / maximum;
}
