/*
 * Deskflow -- HarmonyOS Deskflow client status-bar quick-operation page
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

import { commonEventManager } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { common } from '@kit.AbilityKit';
import { desktopManager } from '../statusbar/DeskflowStatusStore';
import { ConnState } from '../statusbar/StatusBarManager';
import { BGEVT_STATUS, BGEVT_STATUS_REQUEST } from '../background/BackGroundAbility';

const DOMAIN = 0xD002;

@Entry
@Component
struct StatusBarQuickView {
  @State state: number = ConnState.DISCONNECTED;
  @State statusText: string = '未连接';
  // commonEvent 订阅器必须持有引用，否则可能被 GC 回收；
  // aboutToDisappear 时反注册，避免监听器越积越多。
  private bgSub: commonEventManager.CommonEventSubscriber | null = null;
  private stateListener: ((s: number, t: string) => void) | null = null;

  aboutToAppear(): void {
    this.stateListener = (s: number, t: string) => {
      this.state = s;
      this.statusText = t;
    };
    desktopManager.registerStateListener(this.stateListener);
    this.state = desktopManager.getState();
    this.statusText = desktopManager.getStatusText();
    // 托盘快览扩展与主进程不在同一进程，模块级单例 desktopManager
    // 并不共享，之前这里永远显示初始值“未连接”。改走后台广播拿状态。
    try {
      if (!this.bgSub) {
        const context = this.getUIContext().getHostContext() as common.Context;
        const bundleName = context.applicationInfo.name;
        const info: commonEventManager.CommonEventSubscribeInfo = {
          events: [BGEVT_STATUS], publisherBundleName: bundleName
        };
        this.bgSub = commonEventManager.createSubscriberSync(info);
        commonEventManager.subscribe(this.bgSub, (err, data) => {
          if (err || !data?.data) {
            return;
          }
          try {
            const parsed = JSON.parse(`${data.data}`) as Record<string, string>;
            if (parsed.code === 'tls_identity') return;
            if (typeof parsed.message === 'string') {
              this.statusText = parsed.message;
            }
            switch (parsed.code) {
              case 'connecting':
                this.state = ConnState.CONNECTING;
                break;
              case 'connected':
                this.state = ConnState.CONNECTED;
                break;
              case 'error':
                this.state = ConnState.ERROR;
                break;
              case 'disconnected':
                this.state = ConnState.DISCONNECTED;
                break;
              default:
                break;
            }
          } catch (e) {
            hilog.warn(DOMAIN, 'DeskflowStatusBar', 'quick view status parse fail: %{public}s', JSON.stringify(e));
          }
        });
        commonEventManager.publish(BGEVT_STATUS_REQUEST, { data: 'state', bundleName: bundleName }, (err) => {
          if (err) hilog.warn(DOMAIN, 'DeskflowStatusBar', 'status request fail: %{public}s', JSON.stringify(err));
        });
      }
    } catch (e) {
      hilog.warn(DOMAIN, 'DeskflowStatusBar', 'quick view subscribe fail: %{public}s', JSON.stringify(e));
    }
  }

  aboutToDisappear(): void {
    if (this.stateListener) {
      desktopManager.unregisterStateListener(this.stateListener);
      this.stateListener = null;
    }
    if (this.bgSub) {
      try {
        commonEventManager.unsubscribe(this.bgSub);
      } catch (e) {
        // 忽略反注册失败
      }
      this.bgSub = null;
    }
  }

  build() {
    Column({ space: 10 }) {
      Text('Deskflow')
        .fontSize(16)
        .fontWeight(FontWeight.Bold)
      Text(this.statusText)
        .fontSize(13)
        .fontColor(
          this.state === ConnState.CONNECTED ? '#4CB050' : this.state === ConnState.ERROR ? '#F44336' : '#666666'
        )
      Text('点右键菜单可：打开主界面 / 连接 / 断开')
        .fontSize(11)
        .fontColor('#999999')
        .textAlign(TextAlign.Center)
    }
    .width('100%')
    .padding(16)
    .alignItems(HorizontalAlign.Center)
  }
}
