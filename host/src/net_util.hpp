// Villen host — host-address discovery for the join URL shown to players.
#pragma once

#include <string>
#include <vector>

namespace villen::net {

// Non-loopback IPv4 addresses of this machine, best-effort. Used to print/show
// the "ws://<ip>:<port>" players type or scan (DESIGN §3.5, §10.6).
std::vector<std::string> localIpv4Addresses();

}  // namespace villen::net
