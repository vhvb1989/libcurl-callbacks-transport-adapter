/**
 * Definition of my own transport adapter using libcurl
 */

#include <azure/core/http/transport.hpp>

namespace MyNameSpace
{

    class MyTransport final : public Azure::Core::Http::HttpTransport
    {
        std::unique_ptr<Azure::Core::Http::RawResponse> Send(Azure::Core::Http::Request &request, Azure::Core::Context const &context) override;
    };
}
