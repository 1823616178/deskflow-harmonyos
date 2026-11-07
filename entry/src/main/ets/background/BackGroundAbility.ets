/*
 * Deskflow -- HarmonicOHOS Deskflow client -- background connection carrier
 * 以独立进程承载 Deskflow 连接原生 worker（processMode=1 NEW_PROCESS_ATTACH_TO_PARENT）。
 * 主界面隐藏到托盘时，主 Ability 进入后台，但承载进程保持 FOREGROUND，
 * 因此 TCP 连接与自动重连仍持续运行，无需重新打开主界面。
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

import { AbilityConstant, UIAbility, Want } from '@kit.AbilityKit';
import { commonEventManager } from '@kit.BasicServicesKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { display } from '@kit.ArkUI';
import deskflow from 'libentry.so';
import { ClipboardAssembler, ClipboardChunk, splitClipboardText } from './ClipboardTransfer';

const DOMAIN = 0x0000;
const TAG = 'DeskflowBG';

/** 广播给主窗口页面的事件名（与 Index.ets 保持一致） */
export const BGEVT_STATUS = 'deskflow.bg.status';
export const BGEVT_CLIP = 'deskflow.bg.clip';
export const BGEVT_STOP = 'deskflow.bg.stop';
export const BGEVT_CLIP_PUSH = 'deskflow.bg.clip.push';
/** 主窗口下发设置变更（滚轮反转/自动重连/剪贴板开关）给承载进程 */
export const BGEVT_SETTINGS = 'deskflow.bg.settings';
export const BGEVT_READY = 'deskflow.bg.ready';
export const BGEVT_STATUS_REQUEST = 'deskflow.bg.status.request';

export default class BackGroundAbility extends UIAbility {
  // 订阅器必须持有引用，否则可能被 GC 回收导致收不到 commonEvent。
  // 之前这里的 stop/push 订阅器只是 wireEvents() 里的局部变量。
  private stopSub: commonEventManager.CommonEventSubscriber | null = null;
  private pushSub: commonEventManager.CommonEventSubscriber | null = null;
  private settingsSub: commonEventManager.CommonEventSubscriber | null = null;
  private statusRequestSub: commonEventManager.CommonEventSubscriber | null = null;
  private lastStatus: string = JSON.stringify({ code: 'disconnected', message: '未连接' });
  // 分片剪贴板重组：id -> {parts, total, received, receivedAt}
  private clipChunks: ClipboardAssembler = new ClipboardAssembler();

  onCreate(want: Want, launchParam: AbilityConstant.LaunchParam): void {
    hilog.info(DOMAIN, TAG, 'BackGroundAbility onCreate');
    this.wireEvents();
    const host = want.parameters?.['host'] as string;

    if (!host) {
      hilog.info(DOMAIN, TAG, 'no host in want, running as pure carrier');
      this.publish({ event: BGEVT_READY, data: 'ready' });
      return;
    }
    const port = Number(want.parameters?.['port'] ?? 24800);
    const screen = (want.parameters?.['screen'] as string) ?? 'HarmonyPC';
    const invert = want.parameters?.['invert'] === true;
    const autoReconnect = want.parameters?.['autoreconnect'] !== false;
    const clipboard = want.parameters?.['clipboard'] === true;

    try {
      const d = display.getDefaultDisplaySync();
      const w = Math.round(d.width);
      const h = Math.round(d.height);
      deskflow.setInvertScroll(invert);
      deskflow.setAutoReconnect(autoReconnect);
      deskflow.setClipboardSync(clipboard);
      deskflow.connectDeskflow(host, port, screen, w, h);
      hilog.info(DOMAIN, TAG, 'connect %{public}s:%{public}d', host, port);
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'connect/push failed: %{public}s', JSON.stringify(e));
      const err = e as BusinessError;
      this.publish({ event: BGEVT_STATUS,
        data: JSON.stringify({ code: 'error', message: `connect failed (${err.code}): ${err.message}` }) });
    }
    this.publish({ event: BGEVT_READY, data: 'ready' });
  }

  /**
   * 把原生层的状态字符串映射为结构化 code，再以 JSON 广播。
   * 之前 Index.ets 靠 indexOf('error'/'reconnect'/...) 猜状态，
   * 其中 'reconnecting in 3000ms' 会被误判为 CONNECTED（托盘变绿）。
   * code: connecting | connected | error | disconnected | unknown
   */
  private statusCode(msg: string): string {
    const t = (msg ?? '').trim().toLowerCase();
    if (t.indexOf('error') >= 0 || t.indexOf('closed') >= 0 || t.indexOf('incompatible') >= 0 ||
      t.indexOf('unknown') >= 0 || t.indexOf('violation') >= 0) {
      return 'error';
    }
    if (t.indexOf('reconnecting') >= 0 || t.indexOf('connecting') >= 0 ||
      t.indexOf('connect failed') >= 0) {
      return 'connecting';
    }
    if (t.indexOf('active') >= 0 || t.indexOf('control acquired') >= 0 ||
      t.indexOf('handshake ok') >= 0 || t.indexOf('waiting for enter') >= 0) {
      return 'connected';
    }
    if (t.indexOf('disconnected') >= 0) {
      return 'disconnected';
    }
    return 'unknown';
  }

  private wireEvents(): void {
    try {
      deskflow.onDeskflowStatus((event: deskflow.ValueChangeEvent) => {
        const msg = event.value ?? '';
        const payload = JSON.stringify({ code: this.statusCode(msg), message: msg });
        this.lastStatus = payload;
        const data: commonEventManager.CommonEventData = { event: BGEVT_STATUS, data: payload };
        this.publish(data);
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'status cb fail: %{public}s', JSON.stringify(e));
    }
    try {
      deskflow.onClipboardData((event: deskflow.ValueChangeEvent) => {
        hilog.info(DOMAIN, TAG, 'onClipboardData recv len=%{public}d, publish BGEVT_CLIP', (event.value ?? '').length);
        const id = `remote-${Date.now()}-${Math.floor(Math.random() * 1000000)}`;
        for (const chunk of splitClipboardText(event.value ?? '', id)) {
          this.publish({ event: BGEVT_CLIP, data: JSON.stringify(chunk) });
        }
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'clip cb fail: %{public}s', JSON.stringify(e));
    }
    // 订阅「断开」事件：主窗口触发时停止后台承载
    try {
      if (!this.stopSub) {
        const info: commonEventManager.CommonEventSubscribeInfo = {
          events: [BGEVT_STOP], publisherBundleName: this.context.abilityInfo.bundleName
        };
        this.stopSub = commonEventManager.createSubscriberSync(info);
        commonEventManager.subscribe(this.stopSub, (err, data) => {
          if (!err) {
            hilog.info(DOMAIN, TAG, 'stop request received');
            this.context.terminateSelf().catch((e: BusinessError) => {
              hilog.error(DOMAIN, TAG, 'terminateSelf fail: %{public}s', JSON.stringify(e));
            });
          }
        });
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'stop sub fail: %{public}s', JSON.stringify(e));
    }
    // 订阅「推送本机剪贴板」：主窗口把本机剪贴板文本发来，由本进程（持有连接）推给服务端（鸿蒙→Windows）
    try {
      if (!this.pushSub) {
        const pushInfo: commonEventManager.CommonEventSubscribeInfo = {
          events: [BGEVT_CLIP_PUSH], publisherBundleName: this.context.abilityInfo.bundleName
        };
        this.pushSub = commonEventManager.createSubscriberSync(pushInfo);
        commonEventManager.subscribe(this.pushSub, (err, data) => {
          if (!err && data?.data) {
            // 分片文本先重组，组装完成才推；单片文本直接推。
            const text = this.reassembleClipChunk(`${data.data}`);
            if (text === null) {
              return; // 还在等后续分片
            }
            hilog.info(DOMAIN, TAG, 'clip push request len=%{public}d', text.length);
            try {
              // 这是用户已授权的显式推送（PasteButton 点击或 UI 侧已门控的
              // 剪贴板变化），直接推，不再无条件 setClipboardSync(true)
              // 绕过用户在 UI 上关掉的开关。
              deskflow.pushClipboard(text);
              hilog.info(DOMAIN, TAG, 'clip pushed to server ok');
            } catch (e) {
              hilog.error(DOMAIN, TAG, 'pushClipboard fail: %{public}s', JSON.stringify(e));
            }
          }
        });
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'clip push sub fail: %{public}s', JSON.stringify(e));
    }
    // 订阅「设置变更」：主窗口改开关后，把设置同步到本进程的原生客户端。
    // 之前 UI 进程直接调 deskflow.set*，调的是 UI 进程自己的空客户端，
    // 连接后改开关当次完全不生效。
    try {
      if (!this.settingsSub) {
        const settingsInfo: commonEventManager.CommonEventSubscribeInfo = {
          events: [BGEVT_SETTINGS], publisherBundleName: this.context.abilityInfo.bundleName
        };
        this.settingsSub = commonEventManager.createSubscriberSync(settingsInfo);
        commonEventManager.subscribe(this.settingsSub, (err, data) => {
          if (err || !data?.data) {
            return;
          }
          try {
            const settings = JSON.parse(`${data.data}`) as Record<string, boolean>;
            if (typeof settings.invert === 'boolean') {
              deskflow.setInvertScroll(settings.invert);
            }
            if (typeof settings.autoReconnect === 'boolean') {
              deskflow.setAutoReconnect(settings.autoReconnect);
            }
            if (typeof settings.clipboard === 'boolean') {
              deskflow.setClipboardSync(settings.clipboard);
            }
            hilog.info(DOMAIN, TAG, 'settings applied: %{public}s', `${data.data}`);
          } catch (e) {
            hilog.error(DOMAIN, TAG, 'settings parse fail: %{public}s', JSON.stringify(e));
          }
        });
      }
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'settings sub fail: %{public}s', JSON.stringify(e));
    }
    try {
      const info: commonEventManager.CommonEventSubscribeInfo = {
        events: [BGEVT_STATUS_REQUEST], publisherBundleName: this.context.abilityInfo.bundleName
      };
      this.statusRequestSub = commonEventManager.createSubscriberSync(info);
      commonEventManager.subscribe(this.statusRequestSub, (err) => {
        if (!err) this.publish({ event: BGEVT_STATUS, data: this.lastStatus });
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'status request sub fail: %{public}s', JSON.stringify(e));
    }
  }

  /**
   * 分片剪贴板重组：收到完整分片返回组装后的文本；收到分片但未齐时
   * 返回 null；收到非分片文本直接原样返回。超时 30s 未齐的分片丢弃。
   */
  private reassembleClipChunk(raw: string): string | null {
    try {
      const obj = JSON.parse(raw) as ClipboardChunk;
      if (obj.clipChunk === true) {
        return this.clipChunks.accept(obj);
      }
    } catch (e) {
      // Compatibility with an older UI process sending plain text.
    }
    return raw;
  }

  private publish(msg: commonEventManager.CommonEventData): void {
    try {
      const opts: commonEventManager.CommonEventPublishData = {
        data: msg.data, bundleName: this.context.abilityInfo.bundleName
      };
      commonEventManager.publish(msg.event, opts, (err) => {
        if (err) {
          hilog.error(DOMAIN, TAG, 'publish %{public}s fail: %{public}s', msg.event, JSON.stringify(err));
        }
      });
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'publish %{public}s error: %{public}s', msg.event, JSON.stringify(e));
    }
  }

  onDestroy(): void {
    hilog.info(DOMAIN, TAG, 'BackGroundAbility onDestroy, stopping client');
    try {
      deskflow.disconnectDeskflow();
    } catch (e) {
      hilog.error(DOMAIN, TAG, 'disconnect fail: %{public}s', JSON.stringify(e));
    }
    // 反注册，避免残留订阅器
    try {
      if (this.stopSub) {
        commonEventManager.unsubscribe(this.stopSub);
        this.stopSub = null;
      }
      if (this.pushSub) {
        commonEventManager.unsubscribe(this.pushSub);
        this.pushSub = null;
      }
      if (this.settingsSub) {
        commonEventManager.unsubscribe(this.settingsSub);
        this.settingsSub = null;
      }
      if (this.statusRequestSub) {
        commonEventManager.unsubscribe(this.statusRequestSub);
        this.statusRequestSub = null;
      }
      this.clipChunks.clear();
    } catch (e) {
      hilog.warn(DOMAIN, TAG, 'unsubscribe fail: %{public}s', JSON.stringify(e));
    }
  }
}
