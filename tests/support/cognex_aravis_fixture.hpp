#pragma once
#include <arv.h>
#include <stdexcept>

// Original test arrangement through public Aravis DOM, not vendor XML or
// physical-camera evidence. Retain the Fake's actual period-register converter.
namespace cognex_aravis {
inline ArvDomNode* add(ArvDevice* d, ArvDomNode* parent, const char* tag,
                       const char* name = nullptr, const char* text = nullptr) {
    auto* doc = ARV_DOM_DOCUMENT(arv_device_get_genicam(d));
    if (!parent) parent = ARV_DOM_NODE(arv_dom_document_get_document_element(doc));
    auto* node = ARV_DOM_NODE(arv_dom_document_create_element(doc, tag));
    if (!arv_dom_node_append_child(parent, node)) throw std::runtime_error("Cognex fixture DOM insertion failed");
    if (name) arv_dom_element_set_attribute(ARV_DOM_ELEMENT(node), "Name", name);
    if (text) arv_dom_node_append_child(node, ARV_DOM_NODE(arv_dom_document_create_text_node(doc, text)));
    return node;
}
inline void string(ArvDevice* d, const char* name, const char* value) {
    add(d, add(d, nullptr, "String", name), "Value", nullptr, value);
}
inline void arrange(ArvDevice* d) {
    string(d, "DeviceVendorName", "COGNEX");
    string(d, "DeviceModelName", "CIC-5000R");
    string(d, "DeviceFirmwareVersion", "106349-08;U;CIC-5000R;V3.7-1;0");
    add(d, add(d, ARV_DOM_NODE(arv_device_get_feature(d, "PixelFormat")), "EnumEntry", "Mono12"),
        "Value", nullptr, "17825797"); // fixture representation, never hardware byte-order evidence
    auto* rate = arv_device_get_feature(d, "AcquisitionFrameRate");
    arv_dom_element_set_attribute(ARV_DOM_ELEMENT(rate), "Name", "AcquisitionFrameRateAbs");
    add(d, add(d, nullptr, "Integer", "CognexRateEnableValue"), "Value", nullptr, "1");
    add(d, add(d, nullptr, "Boolean", "AcquisitionFrameRateEnable"), "pValue", nullptr, "CognexRateEnableValue");
    GError* error = nullptr;
    arv_device_set_float_feature_value(d, "AcquisitionFrameRateAbs", 10, &error);
    if (error) { const std::string message = error->message; g_error_free(error); throw std::runtime_error(message); }
}
inline void rate(ArvDevice* d, double value) {
    GError* error = nullptr;
    arv_device_set_float_feature_value(d, "AcquisitionFrameRateAbs", value, &error);
    if (error) { const std::string message = error->message; g_error_free(error); throw std::runtime_error(message); }
}
} // namespace cognex_aravis