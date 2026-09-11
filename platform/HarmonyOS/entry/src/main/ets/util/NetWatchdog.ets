/*
 * Network change watchdog — HarmonyOS counterpart of the Android demo's
 * NetWatchdog (which listens for the CONNECTIVITY_ACTION broadcast).
 *
 * The Android behaviour it reproduces:
 *  - wifi -> cellular  : pause playback and ask the user (NetChangeView)
 *  - cellular -> wifi  : dismiss that prompt, and if the retry budget is spent
 *                        reset it to 3 and reload the source
 *  - disconnected      : only remember that a retry will be needed; Android
 *                        notes that handovers report a disconnect first
 *
 * Android classifies the link with ConnectivityManager.TYPE_WIFI / TYPE_MOBILE;
 * here the default network's bearer types are inspected instead.
 */

import { connection } from '@kit.NetworkKit';

export interface NetChangeListener {
  onWifiTo4G?: () => void;
  on4GToWifi?: () => void;
  onNetDisconnected?: () => void;
}

export class NetWatchdog {
  private netConnection: connection.NetConnection | null = null;
  private listener: NetChangeListener = {};
  private isWifi: boolean = false;
  private hasFirstReport: boolean = false;

  start(listener: NetChangeListener): void {
    this.listener = listener;
    try {
      this.netConnection = connection.createNetConnection();
      this.netConnection.register((error) => {
        if (error) {
          return;
        }
        this.refresh();
      });
      this.netConnection.on('netAvailable', () => this.refresh());
      this.netConnection.on('netLost', () => this.onDisconnected());
      this.netConnection.on('netCapabilitiesChange', () => this.refresh());
    } catch (err) {
      this.netConnection = null;
    }
  }

  stop(): void {
    if (this.netConnection === null) {
      return;
    }
    try {
      this.netConnection.unregister((error) => {
      });
    } catch (err) {
      // Ignore: nothing useful to do while tearing down.
    }
    this.netConnection = null;
  }

  private refresh(): void {
    try {
      const handle: connection.NetHandle = connection.getDefaultNetSync();
      if (handle === undefined || handle === null || handle.netId === 0) {
        this.onDisconnected();
        return;
      }
      const caps: connection.NetCapabilities = connection.getNetCapabilitiesSync(handle);
      if (caps === undefined || caps === null || caps.bearerTypes === undefined) {
        return;
      }
      let wifi: boolean = false;
      for (const bearer of caps.bearerTypes) {
        if (bearer === connection.NetBearType.BEARER_WIFI) {
          wifi = true;
        }
      }
      if (!this.hasFirstReport) {
        // Do not fire a transition for the initial reading.
        this.hasFirstReport = true;
        this.isWifi = wifi;
        return;
      }
      if (wifi === this.isWifi) {
        return;
      }
      this.isWifi = wifi;
      if (wifi) {
        if (this.listener.on4GToWifi) {
          this.listener.on4GToWifi();
        }
      } else {
        if (this.listener.onWifiTo4G) {
          this.listener.onWifiTo4G();
        }
      }
    } catch (err) {
      // Treat an inspection failure as "no usable network".
      this.onDisconnected();
    }
  }

  private onDisconnected(): void {
    if (this.listener.onNetDisconnected) {
      this.listener.onNetDisconnected();
    }
  }
}
