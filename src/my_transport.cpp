#include "my_transport.hpp"

#include <curl/curl.h>

#include <memory>
#include <vector>

using namespace Azure::Core::Http;
using namespace Azure::Core;

namespace
{
    class CurlSession final : public Azure::Core::IO::BodyStream
    {
    private:
        CURL *m_curlHandle;
        struct curl_slist *m_headerHandle = NULL;
        std::vector<uint8_t> m_responseData;
        std::vector<uint8_t> m_sendBuffer;
        std::unique_ptr<RawResponse> m_response = nullptr;
        std::unique_ptr<Azure::Core::IO::BodyStream> m_responseStream;
        bool m_chunked = false;

        // ----- BodyStream implementation ( overrides )   ---- //
        size_t OnRead(uint8_t *buffer, size_t count, Azure::Core::Context const &context) override
        {
            return m_responseStream->Read(buffer, count, context);
        }

        int64_t Length() const override
        {
            return m_chunked ? -1 : m_responseStream->Length();
        }

        void Rewind() override { return m_responseStream->Rewind(); }

        // util functions
        template <class T = int>
        static T GetNextToken(
            char const **begin,
            char const *const last,
            char const separator,
            std::function<T(std::string)> mutator = [](std::string const &value)
            { return std::stoi(value); })
        {
            auto start = *begin;
            auto end = std::find(start, last, separator);
            // Move the original ptr to one place after the separator
            *begin = end + 1;
            return mutator(std::string(start, end));
        }

        constexpr static const int HttpWordLen = 4;
        static std::unique_ptr<RawResponse> CreateHTTPResponse(
            char const *const begin,
            char const *const last)
        {
            // set response code, HTTP version and reason phrase (i.e. HTTP/1.1 200 OK)
            auto start = begin + HttpWordLen + 1; // HTTP = 4, / = 1, moving to 5th place for version
            auto majorVersion = GetNextToken(&start, last, '.');
            auto minorVersion = GetNextToken(&start, last, ' ');
            auto statusCode = GetNextToken(&start, last, ' ');
            auto reasonPhrase = GetNextToken<std::string>(
                &start, last, '\r', [](std::string const &value)
                { return value; });

            // allocate the instance of response to heap with shared ptr
            // So this memory gets delegated outside CurlTransport as a shared_ptr so memory will be
            // eventually released
            return std::make_unique<RawResponse>(
                static_cast<uint16_t>(majorVersion),
                static_cast<uint16_t>(minorVersion),
                HttpStatusCode(statusCode),
                reasonPhrase);
        }

        static void StaticSetHeader(
            Azure::Core::Http::RawResponse &response,
            char const *const first,
            char const *const last)
        {
            // get name and value from header
            auto start = first;
            auto end = std::find(start, last, ':');

            if ((last - first) == 2 && *start == '\r' && *(start + 1) == '\n')
            {
                // Libcurl gives the end of headers as `\r\n`, we just ignore it
                return;
            }

            if (end == last)
            {
                throw std::invalid_argument("Invalid header. No delimiter ':' found.");
            }

            // Always toLower() headers
            auto headerName = Azure::Core::_internal::StringExtensions::ToLower(std::string(start, end));
            start = end + 1; // start value
            while (start < last && (*start == ' ' || *start == '\t'))
            {
                ++start;
            }

            end = std::find(start, last, '\r');
            auto headerValue = std::string(start, end); // remove \r

            response.SetHeader(headerName, headerValue);
        }

        // ------   libcurl callbacks
        static size_t ReceiveInitialResponse(char *contents, size_t size, size_t nmemb, void *userp)
        {
            size_t const expectedSize = size * nmemb;
            std::unique_ptr<RawResponse> *rawResponse = static_cast<std::unique_ptr<RawResponse> *>(userp);

            // First response
            if (*rawResponse == nullptr)
            {
                // parse header to get init data
                *rawResponse = CreateHTTPResponse(contents, contents + expectedSize);
            }
            else
            {
                StaticSetHeader(*(*rawResponse), contents, contents + expectedSize);
            }

            // This callback needs to return the response size or curl will consider it as it failed
            return expectedSize;
        }

        static size_t ReceiveData(void *contents, size_t size, size_t nmemb, void *userp)
        {
            size_t const expectedSize = size * nmemb;
            std::vector<uint8_t> &rawResponse = *(static_cast<std::vector<uint8_t> *>(userp));
            uint8_t *data = static_cast<uint8_t *>(contents);

            rawResponse.insert(rawResponse.end(), data, data + expectedSize);

            // This callback needs to return the response size or curl will consider it as it failed
            return expectedSize;
        }

        static size_t UploadData(void *dst, size_t size, size_t nmemb, void *userdata)
        {
            // Calculate the size of the *dst buffer
            auto destSize = nmemb * size;
            Azure::Core::IO::BodyStream *uploadStream = static_cast<Azure::Core::IO::BodyStream *>(userdata);

            // Terminate the upload if the destination buffer is too small
            if (destSize < 1)
            {
                throw std::runtime_error("Not enough size to continue to upload data.");
            }

            // Copy as many bytes as possible from the stream to libcurl's destination buffer
            return uploadStream->Read(static_cast<uint8_t *>(dst), destSize);
        }

    public:
        CurlSession()
        {
            m_curlHandle = curl_easy_init();
            if (!m_curlHandle)
            {
                throw std::runtime_error("Could not create a new libcurl handle");
            }
        }

        ~CurlSession()
        {
            // Avoid leaks
            if (m_curlHandle)
            {
                curl_easy_cleanup(m_curlHandle);
            }
        }

        std::unique_ptr<RawResponse> Send(Request &request, Context const &context)
        {
            // optional
            context.ThrowIfCancelled();

            {
                // 1.- Parse request into libcurl
                auto const &url = request.GetUrl();
                auto port = url.GetPort();

                // 2.- Perform network call
                CURLcode operationResult;
                //url
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_URL, url.GetAbsoluteUrl().data());
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set URL for libcurl");
                }
                //port
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_PORT, port);
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set Port for libcurl");
                }
                // headers
                auto const &headers = request.GetHeaders();
                if (headers.size() > 0)
                {
                    for (auto const &header : headers)
                    {
                        auto newHandle = curl_slist_append(m_headerHandle, (header.first + ":" + header.second).c_str());
                        if (newHandle == NULL)
                        {
                            throw std::runtime_error("Failing creating header list for libcurl");
                        }
                        m_headerHandle = newHandle;
                    }
                    // Add header list to handle
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_HTTPHEADER, m_headerHandle);
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set Port for libcurl");
                    }
                }

                // libcurl callbacks
                // Headers
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_HEADERFUNCTION, ReceiveInitialResponse);
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set Header Function for libcurl");
                }
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_HEADERDATA, static_cast<void *>(&m_response));
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set Header Function Data for libcurl");
                }

                // Receive data
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_WRITEFUNCTION, ReceiveData);
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set Receive Function for libcurl");
                }
                operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_WRITEDATA, static_cast<void *>(&m_responseData));
                if (operationResult != CURLE_OK)
                {
                    throw std::runtime_error("Could not set Receive Function Data for libcurl");
                }

                // libcurl Http Metod
                auto const &method = request.GetMethod();
                if (method == Azure::Core::Http::HttpMethod::Delete)
                {
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_CUSTOMREQUEST, "DELETE");
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set Custom DELETE for libcurl");
                    }
                }
                else if (method == Azure::Core::Http::HttpMethod::Patch)
                {
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_CUSTOMREQUEST, "PATCH");
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set Custom PATCH for libcurl");
                    }
                }
                else if (method == Azure::Core::Http::HttpMethod::Head)
                {
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_NOBODY, 1L);
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set Head NoBody for libcurl");
                    }
                }
                else if (method == Azure::Core::Http::HttpMethod::Post)
                {
                    // Adds special header "Expect:" for libcurl to avoid sending only headers to server and wait
                    // for a 100 Continue response before sending a PUT method
                    auto newHandle = curl_slist_append(m_headerHandle, "Expect:");
                    if (newHandle == NULL)
                    {
                        throw std::runtime_error("Failing adding Expect header for POST");
                    }
                    m_headerHandle = newHandle;

                    m_sendBuffer = request.GetBodyStream()->ReadToEnd();
                    m_sendBuffer.emplace_back('\0'); // the body is expected to be null terminated
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_POSTFIELDS, reinterpret_cast<char *>(m_sendBuffer.data()));
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set CURLOPT_POSTFIELDS for libcurl");
                    }
                }
                else if (method == Azure::Core::Http::HttpMethod::Put)
                {
                    // As of CURL 7.12.1 CURLOPT_PUT is deprecated.  PUT requests should be made using
                    // CURLOPT_UPLOAD

                    // Adds special header "Expect:" for libcurl to avoid sending only headers to server and wait
                    // for a 100 Continue response before sending a PUT method
                    auto newHandle = curl_slist_append(m_headerHandle, "Expect:");
                    if (newHandle == NULL)
                    {
                        throw Azure::Core::Http::TransportException("Failing adding Expect header for POST");
                    }
                    m_headerHandle = newHandle;

                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_UPLOAD, 1L);
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set CURLOPT_UPLOAD for libcurl");
                    }

                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_READFUNCTION, UploadData);
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set CURLOPT_READFUNCTION for libcurl");
                    }
                    auto uploadStream = request.GetBodyStream();
                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_READDATA, static_cast<void *>(uploadStream));
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set CURLOPT_READDATA for libcurl");
                    }

                    operationResult = curl_easy_setopt(m_curlHandle, CURLOPT_INFILESIZE, static_cast<curl_off_t>(uploadStream->Length()));
                    if (operationResult != CURLE_OK)
                    {
                        throw std::runtime_error("Could not set CURLOPT_INFILESIZE for libcurl");
                    }
                }

                // Perform libcurl transfer
                auto performResult = curl_easy_perform(m_curlHandle);
            }

            // 3.- Create a Azure body stream for the RawResponse
            m_responseStream = std::make_unique<Azure::Core::IO::MemoryBodyStream>(m_responseData);

            // ** special case for Azure Reponse -> size - unknown or chunked
            auto const &responseHeaders = m_response->GetHeaders();
            auto transferEncodingHeader = responseHeaders.find("transfer-encoding");
            if (transferEncodingHeader != responseHeaders.end())
            {
                auto headerValue = transferEncodingHeader->second;
                m_chunked = headerValue.find("chunked") != std::string::npos;
            }

            // 4.- Return the rawResponse
            return std::move(m_response);
        }
    };
}

namespace MyNameSpace
{
    std::unique_ptr<RawResponse> MyTransport::Send(Request &request, Context const &context)
    {
        // Set up.
        auto session = std::make_unique<CurlSession>();
        auto response = session->Send(request, context);
        response->SetBodyStream(std::move(session));
        return response;
    }
}