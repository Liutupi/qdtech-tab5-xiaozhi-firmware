#pragma once
template <class... Args>
inline void EspLogStub(Args&&...) {}
#define ESP_LOGI(...) EspLogStub(__VA_ARGS__)
#define ESP_LOGW(...) EspLogStub(__VA_ARGS__)
