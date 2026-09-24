#pragma once

// local -config-endpoint= fails riot's url checks, VAL 29 without this
namespace PlatformEndpoint
{
    void Install();
}
