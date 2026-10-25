/* The default profile: a ToneRelay box with ToneRelay's own names and its own front page. */
#include "tonerelay.h"

const tonerelay_profile_t TONERELAY_PROFILE = {
    .name = "tonerelay",
    .hostname = "tonerelay",
    .instance = "ToneRelay",
    .service = "_tonerelay",
    .ap_prefix = "ToneRelay",
    .with_id = false,
    .setup_ap_only = false,
    .relay_only_default = false,
    .setup_page = NULL,
};
