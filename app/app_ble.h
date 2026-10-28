/*
 * app_ble.h - Application-level BLE bring-up for CH32V208 + Apache NimBLE.
 *
 * main.c carries a weak `app_ble_start()` default (boot-only bring-up image);
 * linking this translation unit provides the strong symbol that brings up the
 * full NimBLE host + controller stack.
 */
#ifndef APP_BLE_H_
#define APP_BLE_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up NimBLE (controller + host) and start advertising "CH32V208-NIMBLE"
 * with a private GATT service.  Runs in the caller's task context; returns
 * after the LL and host tasks have been spawned (the host task then drives
 * everything through the default event queue).
 */
void app_ble_start(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_BLE_H_ */
