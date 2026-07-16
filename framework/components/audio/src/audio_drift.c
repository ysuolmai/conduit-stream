#include "audio_drift.h"
audio_drift_action_t audio_drift_decide(size_t avail, const audio_drift_cfg_t *cfg){
    if (avail == 0) return AUDIO_DRIFT_NONE;             // underrun → silence path
    if (avail >= cfg->high) return AUDIO_DRIFT_DROP;
    if (avail <= cfg->low)  return AUDIO_DRIFT_DUP;
    return AUDIO_DRIFT_NONE;
}
