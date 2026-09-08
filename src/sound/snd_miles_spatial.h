#pragma once
#include <msslib/mss.h>
#include <cmath>

// Miles 9.3b's point-source path leaves lts_scales[0] uninitialized before
// dividing it by itself. Use its receiver layout with an explicit unit weight.
// KIWI already applies distance attenuation; this function supplies panning.
inline void MSS_SetPointPosition(HDIGDRIVER driver, HSAMPLE sample, const MSSVECTOR3D &position)
{
    const D3DSTATE &layout = driver->D3D;
    MSS_SPEAKER const *speakers = nullptr;
    const int count = AIL_speaker_reverb_levels(driver, nullptr, nullptr, &speakers);
    if (count <= 0 || count > MAX_SPEAKERS || !speakers)
    {
        AIL_set_sample_3D_position(sample, position.x, position.y, position.z);
        return;
    }

    float gains[MAX_SPEAKERS] = {};
    const float x = position.x - layout.listen_position.x;
    const float y = position.y - layout.listen_position.y;
    const float z = position.z - layout.listen_position.z;
    const float distance = std::sqrt(x * x + y * y + z * z);
    if (distance <= 0.0001f || layout.n_receiver_specs == 0)
    {
        for (int i = 0; i < count; ++i)
            gains[i] = std::sqrt(1.0f / count);
    }
    else
    {
        for (int i = 0; i < layout.n_ambient_channels; ++i)
            gains[layout.ambient_channels[i]] = std::sqrt(1.0f / count);

        const float pi = 3.14159265358979323846f;
        float angles[MAX_RECEIVER_SPECS] = {};
        float angleSum = 0;
        for (int i = 0; i < layout.n_receiver_specs; ++i)
        {
            const MSSVECTOR3D &v = layout.receiver_specifications[i].direction;
            const float cosine = (v.x * x + v.y * y + v.z * z) / distance;
            angles[i] = cosine > 0.9999f ? pi : cosine < -0.9999f ? 0 : pi - std::acos(cosine);
            angleSum += angles[i];
        }
        const float power = layout.falloff_power > 0 ? layout.falloff_power : 1;
        for (int i = 0; i < layout.n_receiver_specs && angleSum > 0; ++i)
        {
            const MSS_RECEIVER_LIST &receiver = layout.receiver_specifications[i];
            const float weight = std::sqrt(std::pow(angles[i], power) / angleSum);
            for (int j = 0; j < receiver.n_speakers_affected; ++j)
            {
                const int channel = layout.directional_channels[receiver.speaker_index[j]];
                const float contribution = weight * receiver.speaker_level[j];
                if (channel >= 0)
                    gains[channel] += contribution;
                else
                {
                    // Distribute the virtual rear speaker across real receivers.
                    for (int k = 0; k < layout.n_receiver_specs; ++k)
                    {
                        const MSS_RECEIVER_LIST &other = layout.receiver_specifications[k];
                        for (int n = 0; n < other.n_speakers_affected; ++n)
                        {
                            const int output = layout.directional_channels[other.speaker_index[n]];
                            if (output >= 0)
                                gains[output] += contribution / (2 * layout.n_receiver_specs);
                        }
                    }
                }
            }
        }
        for (int i = 0; i < count; ++i)
        {
            gains[i] /= std::sqrt(power);
            if (gains[i] > 1) gains[i] = 1;
        }
    }
    AIL_set_sample_is_3D(sample, 0);
    AIL_set_sample_speaker_scale_factors(sample, speakers, gains, count);
}
