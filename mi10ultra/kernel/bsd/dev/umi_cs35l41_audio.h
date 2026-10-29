/* CS35L41 signed 11-bit PCM volume, in eighths of a dB. */
#ifndef UMI_CS35L41_AUDIO_H
#define UMI_CS35L41_AUDIO_H
#include <stdint.h>
#define UMI_CS35L41_MAX_ATTENUATION 816u /* -102 dB; lower codes are reserved/mute */
static inline uint32_t umi_cs35l41_volume_register(uint32_t attenuation)
{
    if (attenuation > UMI_CS35L41_MAX_ATTENUATION)
        attenuation = UMI_CS35L41_MAX_ATTENUATION;
    return (((2048u - attenuation) & 0x7ffu) << 3) | 4u;
}
#endif
