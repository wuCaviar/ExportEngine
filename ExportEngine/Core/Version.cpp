#include "Version.h"

using namespace ATHC::EE;

static const std::string s_version = Version::full();
static const std::string s_ts = Version::buildTimestamp();

extern "C" {

const char *EE_getVersion()
{
    return s_version.c_str();
}

const char *EE_getBuildTimestamp()
{
    return s_ts.c_str();
}
}
