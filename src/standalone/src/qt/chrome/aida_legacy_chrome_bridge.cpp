#include "qt/chrome/aida_legacy_chrome_bridge.hpp"

namespace aida::qt::chrome {

legacy_chrome_hooks_t& legacy_chrome_hooks()
{
    static legacy_chrome_hooks_t hooks;
    return hooks;
}

}
