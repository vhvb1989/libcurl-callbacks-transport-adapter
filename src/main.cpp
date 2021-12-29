/**
 * Creating a custom transport adapter for Azure SDK for C++. 
 */
#include <map>

#include <azure/storage/blobs.hpp>

#include "my_transport.hpp"

int main()
{
    // Options are used here to set a custom transport adapter
    Azure::Storage::Blobs::BlobClientOptions options;
    options.Transport.Transport = std::make_shared<MyNameSpace::MyTransport>();
    
    // Create container client using the options
    auto blobContainer = Azure::Storage::Blobs::BlobContainerClient::CreateFromConnectionString("connectionString", "containerName", options);
    blobContainer.CreateIfNotExists();
    
    // Create blob client
    auto blobClient = blobContainer.GetAppendBlobClient("blobName");
    blobClient.CreateIfNotExists();

    return 0;
}