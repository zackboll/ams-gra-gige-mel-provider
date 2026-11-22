#pragma once

// Every upstream pure virtual is implemented. Unsupported surfaces return the
// real MEL errors, never fabricated success or an unfulfilled promise.
#define GIGE_UNSUPPORTED_CALLBACK(Message) \
    ir::Return registerMetadataCallback(std::function<void(ir::Channel&, const Message* const)>) override { \
        return ir::Return::NotSupported; \
    }
