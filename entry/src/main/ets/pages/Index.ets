/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

import { hilog } from '@kit.PerformanceAnalysisKit';
import { preferences } from '@kit.ArkData';
import { common, Want, StartOptions, contextConstant } from '@kit.AbilityKit';
import { pasteboard } from '@kit.BasicServicesKit';
import { commonEventManager } from '@kit.BasicServicesKit';
import { BusinessError } from '@kit.BasicServicesKit';
import { ConnState, StatusBarManager } from '../statusbar/StatusBarManager';
import { desktopManager } from '../statusbar/DeskflowStatusStore';
import { BGEVT_STATUS, BGEVT_CLIP, BGEVT_STOP, BGEVT_CLIP_PUSH, BGEVT_SETTINGS, BGEVT_READY } from '../background/BackGroundAbility';
import { DeskflowStatusData } from '../background/DeskflowEventData';
import { ClipboardAssembler, ClipboardChunk, splitClipboardText } from '../background/ClipboardTransfer';
import { normalizeTlsFingerprint, tlsCertificateNotice, tlsPreferenceKey } from '../background/TlsSettings';

const DOMAIN = 0x0000;
const PREFS_NAME = 'deskflow_config';
const PREFS_HOST = 'host';
const PREFS_PORT = 'port';
const PREFS_NAME_FIELD = 'client_name';
const PREFS_INVERT = 'invert_scroll';
const PREFS_AUTORECONNECT = 'auto_reconnect';
const PREFS_CLIPBOARD = 'clipboard_sync';
const PREFS_TLS = 'tls_enabled';

function stateColor(state: number): string {
  switch (state) {
    case ConnState.CONNECTING: return '#E6A23C';
    case ConnState.CONNECTED: return '#4CB050';
    case ConnState.ERROR: return '#F44336';
    default: return '#8A8A8A';
  }
}

function stateLabel(state: number): string {
  switch (state) {
    case ConnState.CONNECTING: return '连接中';
    case ConnState.CONNECTED: return '已连接';
    case ConnState.ERROR: return '异常断开';
    default: return '未连接';
  }
}

@Entry
@Component
struct Index {
  @State connState: number = ConnState.DISCONNECTED;
  @State statusText: string = '尚未连接 Deskflow 服务端';
  @State hostInput: string = '';
  @State portInput: string = '24800';
  @State screenName: string = 'HarmonyPC';
  @State invertScroll: boolean = false;
  @State autoReconnect: boolean = true; // 默认开启自动重连/启动自动连接
  @State clipboardSync: boolean = true; // 默认开启剪贴板同步
  @State tlsEnabled: boolean = true;
  @State tlsFingerprint: string = '';
  @State tlsClientFingerprint: string = '';
  private activeTlsPeer: string = '';
  private tlsPromptVisible: boolean = false;
  @State clipLast: string = ''; // 最近一次读取的本机剪贴板内容（按钮读取结果提示）
  // 剪贴板桥接：避免把刚写入的本机内容又当成本机新复制推回去（回环）
  private lastRemoteClipboard: string = '';
  private remoteClipChunks: ClipboardAssembler = new ClipboardAssembler();
  // 后台承载 Ability 事件订阅器；必须持有引用，否则可能被 GC 回收导致收不到 commonEvent
  private bgSub: commonEventManager.CommonEventSubscriber | null = null;
  private startingConnection: boolean = false;
  // pasteboard 'update' 监听去重：aboutToAppear 可能多次执行，重复注册
  // 会导致一次复制推送多次。
  private clipListenerRegistered: boolean = false;
  // Preferences 实例缓存：之前每次 load/save 都新建实例。
  private prefsPromise: Promise<preferences.Preferences> | null = null;

  private appBundleName(): string {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    return context.abilityInfo.bundleName;
  }

  private getPrefs(): Promise<preferences.Preferences> {
    if (!this.prefsPromise) {
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      this.prefsPromise = preferences.getPreferences(context, PREFS_NAME).catch((e: BusinessError) => {
        this.prefsPromise = null;
        return Promise.reject(e);
      });
    }
    return this.prefsPromise;
  }

  /** 把当前三项设置同步到后台承载进程的原生客户端。之前 UI 进程直接
   *  调 deskflow.set*，调的是 UI 进程自己的空客户端，连接后改开关无效。 */
  private publishSettings(): void {
    try {
      const payload = JSON.stringify({
        invert: this.invertScroll,
        autoReconnect: this.autoReconnect,
        clipboard: this.clipboardSync
      });
      const opts: commonEventManager.CommonEventPublishData = { data: payload, bundleName: this.appBundleName() };
      commonEventManager.publish(BGEVT_SETTINGS, opts, (err) => {
        if (err) {
          hilog.warn(DOMAIN, 'DeskflowBg', 'settings publish fail: %{public}s', JSON.stringify(err));
          this.statusText = '设置同步失败，请重试';
          this.syncTray();
        }
      });
    } catch (e) {
      hilog.warn(DOMAIN, 'DeskflowBg', 'publishSettings error: %{public}s', JSON.stringify(e));
      this.statusText = '设置同步失败，请重试';
      this.syncTray();
    }
  }

  /** 校验连接配置：host 非空、端口 1-65535 整数、屏幕名去空格后非空。
   *  失败返回提示文案，通过返回 null。 */
  private validateConfig(): string | null {
    if (this.hostInput.trim().length === 0) {
      return '请先填写服务端 IP/地址';
    }
    const portNum = Number(this.portInput);
    if (!Number.isInteger(portNum) || portNum < 1 || portNum > 65535) {
      return '端口无效，应为 1-65535 的整数';
    }
    if (this.screenName.trim().length === 0) {
      return '请填写屏幕名（需与服务端配置一致）';
    }
    if (this.screenName.trim().length > 64) {
      return '屏幕名过长（最多 64 字符）';
    }
    if (this.tlsEnabled && normalizeTlsFingerprint(this.tlsFingerprint) === null) {
      return '服务端指纹应为 64 位 SHA-256 十六进制值（可带冒号）';
    }
    return null;
  }

  // 后台承载进程已把状态映射为结构化 code（见 BackGroundAbility.statusCode），
  // 这里按 code 切换，不再靠字符串包含猜测。之前 'reconnecting in 3000ms'
  // 含 'reconnect' 会被误判为 CONNECTED，托盘在未连上时变绿。
  private mapStatusCode(code: string): number {
    switch (code) {
      case 'connecting': return ConnState.CONNECTING;
      case 'connected': return ConnState.CONNECTED;
      case 'error': return ConnState.ERROR;
      case 'disconnected': return ConnState.DISCONNECTED;
      default: return this.connState;
    }
  }

  // 解析后台广播的状态 payload：JSON {code, message}；解析失败时按原样
  // 当作纯文本消息显示，状态保持当前值（兼容旧版承载进程）。
  private parseStatusPayload(raw: string): DeskflowStatusData {
    try {
      const parsed = JSON.parse(raw) as Record<string, string>;
      if (typeof parsed.code === 'string' && typeof parsed.message === 'string') {
        return { code: parsed.code, message: parsed.message, peer: parsed.peer };
      }
    } catch (e) {
      // 非 JSON：旧版纯文本状态
    }
    return { code: '', message: raw };
  }

  aboutToAppear(): void {
    // 托盘菜单回调
    const sb = StatusBarManager.getInstance();
    sb.onConnectClick = () => this.onConnect();
    sb.onDisconnectClick = () => this.onDisconnect();
    // 连接由后台承载 Ability 持有；状态/剪贴板经 commonEvent 广播回本页
    this.subscribeBgEvents();
    // 监听本机剪贴板变化：检测到真实的本地复制时立即推给服务端。
    // 用 pasteboard 'update' 监听比等 COUT 离开屏幕更可靠（服务端离开时不一定发 COUT 给我们）。
    // 注意：无 READ_PASTEBOARD 时 getDataSync() 读不到明文，此监听只能在
    // PasteButton 临时授权窗口内真正推走文本，见 pushLocalClipboard 注释。
    if (!this.clipListenerRegistered) {
      try {
        pasteboard.getSystemPasteboard().on('update', () => {
          if (this.clipboardSync) {
            this.pushLocalClipboard().catch((e: BusinessError) => {
              this.statusText = `剪贴板同步失败: ${e.message}`;
              hilog.warn(DOMAIN, 'DeskflowClip', 'clipboard publish fail: %{public}s', e.message);
            });
          }
        });
        this.clipListenerRegistered = true;
      } catch (e) {
        hilog.warn(DOMAIN, 'DeskflowClip', 'register pasteboard update listener fail: %{public}s', JSON.stringify(e));
      }
    }
    // 回填上次保存的连接配置
    this.loadPrefs();
  }

  /** 订阅后台承载 Ability 广播的连接状态 / 远程剪贴板 */
  private subscribeBgEvents(): void {
    try {
      if (this.bgSub) {
        return; // 已订阅
      }
      const info: commonEventManager.CommonEventSubscribeInfo =
        { events: [BGEVT_STATUS, BGEVT_CLIP, BGEVT_READY], publisherBundleName: this.appBundleName() };
      const sub = commonEventManager.createSubscriberSync(info);
      this.bgSub = sub; // 持有引用，防止 GC 回收导致订阅失效
      hilog.info(DOMAIN, 'DeskflowBg', 'subscribeBgEvents: created subscriber %{public}s', info.events.join(','));
      commonEventManager.subscribe(sub, (err, data) => {
        if (err) {
          hilog.warn(DOMAIN, 'DeskflowBg', 'subscribe fail: %{public}s', JSON.stringify(err));
          return;
        }
        if (data?.event === BGEVT_READY) {
          this.publishSettings(); // replay changes made while the carrier was starting
        } else if (data?.event === BGEVT_STATUS) {
          const parsed = this.parseStatusPayload(`${data.data ?? ''}`);
          if (parsed.peer && this.activeTlsPeer && parsed.peer !== this.activeTlsPeer) return;
          if (parsed.code === 'tls_identity') {
            if (parsed.peer === this.activeTlsPeer) this.tlsClientFingerprint = parsed.message;
            return;
          }
          this.statusText = parsed.message;
          if (parsed.code) {
            this.connState = this.mapStatusCode(parsed.code);
          }
          this.syncTray();
          this.handleTlsCertificate(parsed);
        } else if (data?.event === BGEVT_CLIP) {
          try {
            const chunk = JSON.parse(`${data.data ?? ''}`) as ClipboardChunk;
            const value = this.remoteClipChunks.accept(chunk);
            if (value !== null && this.clipboardSync) {
              this.lastRemoteClipboard = value;
              this.setSystemClipboard(value);
            }
          } catch (e) {
            hilog.warn(DOMAIN, 'DeskflowClip', 'invalid remote clipboard packet');
          }
        }
      });
    } catch (e) {
      hilog.warn(DOMAIN, 'DeskflowBg', 'subscribeBgEvents error: %{public}s', JSON.stringify(e));
    }
  }

  private handleTlsCertificate(status: DeskflowStatusData): void {
    const notice = tlsCertificateNotice(status.message);
    const peer = tlsPreferenceKey(this.hostInput, Number(this.portInput));
    if (notice === null || !this.tlsEnabled || this.tlsPromptVisible ||
      status.peer !== this.activeTlsPeer || peer !== this.activeTlsPeer) return;
    this.statusText = notice.changed ? '服务端证书已变更，请核对指纹' : '请核验服务端证书';
    this.syncTray();
    this.tlsPromptVisible = true;
    this.getUIContext().showAlertDialog({
      title: notice.changed ? '服务端证书已变更' : '信任服务端证书',
      message: `请与 Windows Deskflow 设置中显示的 SHA-256 证书指纹核对，一致后再信任。\n\n${notice.fingerprint}`,
      primaryButton: { value: '取消', action: () => { this.tlsPromptVisible = false; } },
      secondaryButton: { value: '指纹一致，信任并重连', action: () => {
        this.tlsPromptVisible = false;
        if (tlsPreferenceKey(this.hostInput, Number(this.portInput)) !== peer || !this.tlsEnabled) return;
        this.getPrefs().then(async (pref: preferences.Preferences) => {
          pref.putSync(peer, notice.fingerprint);
          await pref.flush();
          this.tlsFingerprint = notice.fingerprint;
          this.onConnect();
        }).catch((e: Error) => { this.statusText = `保存证书信任失败: ${e.message}`; });
      } },
      cancel: () => { this.tlsPromptVisible = false; }
    });
  }

  /** 读取上次保存的连接配置（host/port/name/滚轮反转/自动重连） */
  private loadPrefs(): void {
    try {
      this.getPrefs().then((pref: preferences.Preferences) => {
        this.hostInput = pref.getSync(PREFS_HOST, '') as string;
        this.portInput = pref.getSync(PREFS_PORT, '24800') as string;
        this.screenName = pref.getSync(PREFS_NAME_FIELD, 'HarmonyPC') as string;
        this.invertScroll = pref.getSync(PREFS_INVERT, false) as boolean;
        this.autoReconnect = pref.getSync(PREFS_AUTORECONNECT, true) as boolean;
        this.clipboardSync = pref.getSync(PREFS_CLIPBOARD, true) as boolean;
        this.tlsEnabled = pref.getSync(PREFS_TLS, true) as boolean;
        this.tlsFingerprint = pref.getSync(tlsPreferenceKey(this.hostInput, Number(this.portInput)), '') as string;
        // 启动自动连接：回填完配置后，若开启且地址可用，自动发起连接
        if (this.autoReconnect && this.hostInput.trim().length > 0 && this.connState === ConnState.DISCONNECTED) {
          hilog.info(DOMAIN, 'DeskflowPrefs', 'auto reconnect on launch, connecting...');
          this.onConnect();
        }
      }).catch((e: Error) => {
        hilog.error(DOMAIN, 'DeskflowPrefs', 'load prefs fail: %{public}s', e.message);
      });
    } catch (err) {
      hilog.error(DOMAIN, 'DeskflowPrefs', 'load pref error: %{public}s', JSON.stringify(err));
    }
  }

  /** 保存连接配置（下次启动自动回填） */
  private savePrefs(): void {
    try {
      this.getPrefs().then((pref: preferences.Preferences) => {
        pref.putSync(PREFS_HOST, this.hostInput);
        pref.putSync(PREFS_PORT, this.portInput);
        pref.putSync(PREFS_NAME_FIELD, this.screenName);
        pref.putSync(PREFS_INVERT, this.invertScroll);
        pref.putSync(PREFS_AUTORECONNECT, this.autoReconnect);
        pref.putSync(PREFS_CLIPBOARD, this.clipboardSync);
        pref.putSync(PREFS_TLS, this.tlsEnabled);
        pref.flush().catch((e: Error) => {
          hilog.error(DOMAIN, 'DeskflowPrefs', 'flush prefs fail: %{public}s', e.message);
        });
      }).catch((e: Error) => {
        hilog.error(DOMAIN, 'DeskflowPrefs', 'save prefs fail: %{public}s', e.message);
      });
    } catch (err) {
      hilog.error(DOMAIN, 'DeskflowPrefs', 'save pref error: %{public}s', JSON.stringify(err));
    }
  }

  private syncTray(): void {
    // 更新托盘图标/菜单 + 共享状态（供工具栏快速操作弹窗显示）
    desktopManager.setState(this.connState, this.statusText);
    StatusBarManager.getInstance().updateState(this.connState);
  }

  // ===== 剪贴板同步 =====

  // 注意：不要在这里请求 ohos.permission.READ_PASTEBOARD。
  // 该权限未在 module.json5 声明（声明会导致安装失败 9568289，
  // 见 docs/TROUBLESHOOTING.md §10），请求未声明的受限权限注定
  // 拿不到，还会把开关偷偷关掉。鸿蒙→Windows 方向只能靠下方的
  // PasteButton 用户点击授权，见 onPasteButtonClick。

  /** 读取系统剪贴板纯文本（供推送用；失败返回空串） */
  private readSystemClipboard(): string {
    try {
      const sys = pasteboard.getSystemPasteboard();
      const data = sys.getDataSync();
      const text = data.getPrimaryText ? data.getPrimaryText() : '';
      return text ? text : '';
    } catch (e) {
      const err = e as BusinessError;
      // 201 = 缺少 READ_PASTEBOARD 权限；非 201 为其它异常
      if (err.code === 201) {
        hilog.warn(DOMAIN, 'DeskflowClip', 'read clipboard denied: need READ_PASTEBOARD (code 201)');
      } else {
        hilog.warn(DOMAIN, 'DeskflowClip', 'read clipboard fail code=%{public}d msg=%{public}s',
          err.code, err.message);
      }
    }
    return '';
  }

  /** PasteButton 点击回调：用户主动点击后获得剪贴板临时授权，读取并推给服务端（鸿蒙→Windows） */
  private async onPasteButtonClick(result: PasteButtonOnClickResult, err?: BusinessError<void>): Promise<void> {
    if (result !== PasteButtonOnClickResult.SUCCESS) {
      const code = err?.code !== undefined ? err.code : '?';
      this.statusText = `此按钮需用户亲自点击才能读取剪贴板（授权失败 ${code}），请重试`;
      hilog.warn(DOMAIN, 'DeskflowClip', 'PasteButton auth fail code=%{public}s msg=%{public}s',
        `${code}`, err?.message ?? '');
      return;
    }
    try {
      // 用户主动点击 Button 授权后，当前可在临时授权窗口内读取系统剪贴板明文（免 READ_PASTEBOARD）
      const text = this.readSystemClipboard();
      this.clipLast = text;
      if (text.length === 0) {
        this.statusText = '剪贴板为空或非纯文本，未同步';
        return;
      }
      if (text === this.lastRemoteClipboard) {
        this.statusText = `剪贴板内容已是最新（${text.length} 字符），无需重复同步`;
        return;
      }
      // 确保同步开启（用户点击即代表期望同步），再把文本推给服务端。
      // 设置要发给后台承载进程，UI 进程的 deskflow.setClipboardSync
      // 调的是本进程空客户端，对真实连接无效。
      if (!this.clipboardSync) {
        this.clipboardSync = true;
        this.publishSettings();
        this.savePrefs();
      }
      await this.pushLocalClipboard(text);
      this.statusText = `已读取本机剪贴板并提交同步（${text.length} 字符）`;
      hilog.info(DOMAIN, 'DeskflowClip', 'PasteButton success, read %{public}d chars', text.length);
    } catch (e) {
      const err2 = e as BusinessError;
      hilog.warn(DOMAIN, 'DeskflowClip', 'PasteButton read fail code=%{public}d msg=%{public}s',
        err2.code, err2.message);
      this.statusText = `剪贴板同步失败: ${err2.message ?? JSON.stringify(e)}`;
    }
  }

  /** 写入系统剪贴板（远程剪贴板文本到达时调用） */
  private setSystemClipboard(value: string): void {
    try {
      if (!this.clipboardSync || !value || value.length === 0) {
        return;
      }
      const data = pasteboard.createData(pasteboard.MIMETYPE_TEXT_PLAIN, value);
      pasteboard.getSystemPasteboard().setDataSync(data);
    } catch (e) {
      hilog.warn(DOMAIN, 'DeskflowClip', 'set clipboard fail: %{public}s', JSON.stringify(e));
    }
  }

  /** 本机剪贴板变化（或离开屏幕 / PasteButton 点击）时：把本机剪贴板推给服务端。
   *  @param givenText 可选，外部已读到的文本（如 PasteButton 临时授权内读到），避免二次读取。
   *  注：通过 commonEvent 把文本交给「已在运行的连接承载进程」去推送，而不是新建实例——
   *  因为连接 native 环境在 BackGroundAbility 常驻进程里，新建实例没有已建立的连接。
   *  鸿蒙 6.1 读取系统剪贴板需 READ_PASTEBOARD（声明会导致安装失败 9568289），
   *  普通 getDataSync 拿不到明文，需由 PasteButton 用户点击授权后才有明文。 */
  private async pushLocalClipboard(givenText?: string): Promise<void> {
    if (!this.clipboardSync) {
      return;
    }
    const text = givenText !== undefined ? givenText : this.readSystemClipboard();
    if (text && text.length > 0 && text !== this.lastRemoteClipboard) {
      await this.publishClipText(text);
    }
  }

  /** 把本机剪贴板文本通过 commonEvent 发给后台承载进程（由它推给服务端） */
  private async publishClipText(text: string): Promise<void> {
    const id = `${Date.now()}-${Math.floor(Math.random() * 1000000)}`;
    const chunks = splitClipboardText(text, id);
    for (const chunk of chunks) {
      const opts: commonEventManager.CommonEventPublishData = {
        data: JSON.stringify(chunk), bundleName: this.appBundleName()
      };
      await new Promise<void>((resolve, reject) => {
        commonEventManager.publish(BGEVT_CLIP_PUSH, opts, (err) => {
          if (err && err.code) reject(err);
          else resolve();
        });
      });
    }
    hilog.info(DOMAIN, 'DeskflowClip', 'push local clipboard len=%{public}d (via BG event)', text.length);
  }

  @Builder
  StatusCard() {
    Column() {
      Row() {
        // 状态圆点
        Circle({ width: 12, height: 12 })
          .fill(stateColor(this.connState))
        Text(stateLabel(this.connState))
          .fontSize(16)
          .fontWeight(FontWeight.Medium)
          .fontColor(stateColor(this.connState))
          .margin({ left: 8 })
        Blank()
        Button('连接')
          .fontSize(13)
          .height(30)
          .enabled(this.connState !== ConnState.CONNECTING)
          .onClick(() => this.onConnect())
        Button('断开')
          .fontSize(13)
          .height(30)
          .margin({ left: 8 })
          .enabled(this.connState === ConnState.CONNECTED || this.connState === ConnState.CONNECTING)
          .onClick(() => this.onDisconnect())
      }
      .width('100%')

      Text(this.statusText)
        .fontSize(13)
        .fontColor('#666666')
        .width('100%')
        .margin({ top: 10 })
        .textAlign(TextAlign.Start)
        .wordBreak(WordBreak.BREAK_ALL)
    }
    .width('100%')
    .padding(16)
    .backgroundColor('#F7F9FC')
    .borderRadius(12)
  }

  @Builder
  ConnConfig() {
    Column() {
      Text('服务端连接')
        .fontSize(14)
        .fontWeight(FontWeight.Bold)
        .width('100%')
      TextInput({ placeholder: 'Server IP (例如 192.168.1.10)', text: this.hostInput })
        .type(InputType.Normal)
        .width('100%')
        .height(42)
        .margin({ top: 10 })
        .enabled(this.connState !== ConnState.CONNECTING)
        .onChange((v: string) => { this.hostInput = v; this.tlsFingerprint = '' })
      TextInput({ placeholder: '端口 (默认 24800)', text: this.portInput })
        .type(InputType.NUMBER_DECIMAL)
        .width('100%')
        .height(42)
        .margin({ top: 8 })
        .enabled(this.connState !== ConnState.CONNECTING)
        .onChange((v: string) => { this.portInput = v; this.tlsFingerprint = '' })
      TextInput({ placeholder: '屏幕名 (在服务端显示的名称)', text: this.screenName })
        .type(InputType.Normal)
        .width('100%')
        .height(42)
        .margin({ top: 8 })
        .onChange((v: string) => { this.screenName = v })
      Row() {
        Text('TLS 加密（推荐）').fontSize(14)
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.tlsEnabled })
          .selectedColor('#4CB050')
          .enabled(this.connState !== ConnState.CONNECTING)
          .onChange((value: boolean) => { this.tlsEnabled = value; this.savePrefs() })
      }
      .width('100%')
      .margin({ top: 12 })
      if (this.tlsEnabled) {
        TextInput({ placeholder: '服务端 SHA-256 指纹（首次连接可留空）', text: this.tlsFingerprint })
          .width('100%')
          .margin({ top: 8 })
          .enabled(this.connState !== ConnState.CONNECTING)
          .onChange((value: string) => { this.tlsFingerprint = value })
        Text('首次连接需核对证书指纹；修改安全设置后请重新连接。')
          .fontSize(11).fontColor('#666666').margin({ top: 4 })
        if (this.tlsClientFingerprint.length > 0) {
          Text(`本机证书 SHA-256：${this.tlsClientFingerprint}`)
            .fontSize(11).fontColor('#666666').copyOption(CopyOptions.LocalDevice)
            .wordBreak(WordBreak.BREAK_ALL).margin({ top: 4 })
        }
      } else {
        Text('明文连接，仅用于可信局域网。修改后请重新连接。')
          .fontSize(11).fontColor('#B26A00').margin({ top: 4 })
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
    .padding(16)
    .backgroundColor('#FFFFFF')
    .borderRadius(12)
  }

  @Builder
  Settings() {
    Column() {
      Text('设置')
        .fontSize(14)
        .fontWeight(FontWeight.Bold)
        .width('100%')
      Row() {
        Text('反转滚轮方向')
          .fontSize(14)
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.invertScroll })
          .selectedColor('#4CB050')
          .onChange((isOn: boolean) => {
            this.invertScroll = isOn;
            this.publishSettings();
            this.statusText = isOn ? '滚轮方向已反转' : '滚轮方向已恢复';
            this.savePrefs();
          })
      }
      .width('100%')
      .margin({ top: 12 })

      Row() {
        Text('自动重连/启动自动连接')
          .fontSize(14)
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.autoReconnect })
          .selectedColor('#4CB050')
          .onChange((isOn: boolean) => {
            this.autoReconnect = isOn;
            this.publishSettings();
            this.statusText = isOn ? '自动重连已开启' : '自动重连已关闭';
            this.savePrefs();
          })
      }
      .width('100%')
      .margin({ top: 12 })
      Text('开启后：应用启动时用上次配置自动连接；连接意外断开后自动重连。')
        .fontSize(12)
        .fontColor('#999999')
        .width('100%')
        .margin({ top: 4 })

      Row() {
        Text('剪贴板同步')
          .fontSize(14)
        Blank()
        Toggle({ type: ToggleType.Switch, isOn: this.clipboardSync })
          .selectedColor('#4CB050')
          .onChange((isOn: boolean) => {
            this.clipboardSync = isOn;
            this.publishSettings();
            this.statusText = isOn ?
              '剪贴板同步已开启：鸿蒙→Windows 需点下方“粘贴”按钮手动同步（系统限制，应用无法自动读取剪贴板）' :
              '剪贴板同步已关闭';
            this.savePrefs();
          })
      }
      .width('100%')
      .margin({ top: 12 })
      Text('服务端→本机：进入本机屏幕时自动同步；本机→服务端：需点下方“粘贴”按钮手动同步（系统限制）。')
        .fontSize(12)
        .fontColor('#999999')
        .width('100%')
        .margin({ top: 4 })

      // 安全组件 PasteButton：用户亲自点击后系统才授予一次剪贴板读取临时授权，
      // 用于"鸿蒙→Windows"方向主动同步（无需申请 READ_PASTEBOARD，普通应用也无法申请）。
      // 注意：安全组件文字为系统固定文案"粘贴"，故下方用说明文字示意其用途。
      Row() {
        PasteButton({ icon: PasteIconStyle.LINES, text: PasteDescription.PASTE })
          .fontSize(14)
          .onClick((_event: ClickEvent, result: PasteButtonOnClickResult, err?: BusinessError<void>) => {
            this.onPasteButtonClick(result, err);
          })
        Text('点击读取本机剪贴板并同步到服务端')
          .fontSize(13)
          .fontColor('#333333')
          .margin({ left: 12 })
      }
      .width('100%')
      .margin({ top: 12 })

      if (this.clipLast.length > 0) {
        Text(`最近读取：${this.clipLast.length > 40 ? this.clipLast.substring(0, 40) + '…' : this.clipLast}`)
          .fontSize(12)
          .fontColor('#666666')
          .width('100%')
          .maxLines(2)
          .textOverflow({ overflow: TextOverflow.Ellipsis })
          .margin({ top: 4 })
      }
    }
    .width('100%')
    .alignItems(HorizontalAlign.Start)
    .padding(16)
    .backgroundColor('#FFFFFF')
    .borderRadius(12)
  }

  async onConnect(): Promise<void> {
    if (this.startingConnection) {
      return;
    }
    this.startingConnection = true;
    try {
      const validationError = this.validateConfig();
      if (validationError !== null) {
        this.statusText = validationError;
        this.connState = ConnState.ERROR;
        this.syncTray();
        return;
      }
      this.screenName = this.screenName.trim();
      this.connState = ConnState.CONNECTING;
      const peer = tlsPreferenceKey(this.hostInput, Number(this.portInput));
      const pref = await this.getPrefs();
      const pin = this.tlsEnabled ? normalizeTlsFingerprint(this.tlsFingerprint || (pref.getSync(peer, '') as string)) : '';
      if (pin === null) throw new Error('保存的服务端 SHA-256 指纹无效');
      if (this.tlsEnabled) this.tlsFingerprint = pin;
      this.activeTlsPeer = peer;
      if (this.tlsEnabled && pin) {
        pref.putSync(peer, pin);
        await pref.flush();
      }
      // 连接由后台承载 Ability（状态栏项钉定进程）建立，隐藏到托盘时不被冻结。
      const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
      const want: Want = {
        bundleName: context.abilityInfo.bundleName,
        abilityName: 'BackGroundAbility',
        parameters: {
          'host': this.hostInput.trim(),
          'port': Number(this.portInput),
          'screen': this.screenName,
          'invert': this.invertScroll,
          'autoreconnect': this.autoReconnect,
          'clipboard': this.clipboardSync,
          'tlsEnabled': this.tlsEnabled,
          'tlsFingerprint': this.tlsEnabled ? pin : ''
        }
      };
      this.connState = ConnState.CONNECTING;
      this.statusText = `正在连接 ${this.hostInput.trim()}:${this.portInput}...`;
      this.syncTray();
      await this.startBg(want);
      this.savePrefs();
    } catch (e) {
      const err = e as BusinessError;
      this.statusText = `连接失败 (${err.code}): ${err.message ?? JSON.stringify(e)}`;
      this.connState = ConnState.ERROR;
      hilog.error(DOMAIN, 'DeskflowBg', 'startAbility fail code=%{public}d message=%{public}s',
        err.code, err.message ?? JSON.stringify(e));
    } finally {
      this.startingConnection = false;
    }
    this.syncTray();
  }

  async onDisconnect(): Promise<void> {
    try {
      // 通过 commonEvent 通知后台承载 Ability 断开并自行终止
      await this.requestBgStop();
      this.statusText = '正在断开连接...';
      this.connState = ConnState.DISCONNECTED;
      this.syncTray();
    } catch (e) {
      this.statusText = `断开失败: ${JSON.stringify(e)}`;
      this.connState = ConnState.ERROR;
      this.syncTray();
    }
  }

  /** 以独立进程方式启动后台承载 Ability（隐藏启动，独立于主窗口进程，避免隐藏时被冻结）。 */
  private async startBg(want: Want): Promise<void> {
    const context = this.getUIContext().getHostContext() as common.UIAbilityContext;
    // 停止广播发出后有限重试，等待旧进程退出，避免吞掉“已运行”错误后永远不连接。
    await this.requestBgStop();
    const options: StartOptions = {
      processMode: contextConstant.ProcessMode.NEW_PROCESS_ATTACH_TO_PARENT,
      startupVisibility: contextConstant.StartupVisibility.STARTUP_HIDE
    };
    for (let attempt = 0; attempt < 10; attempt++) {
      try {
        await context.startAbility(want, options);
        return;
      } catch (e) {
        const err = e as BusinessError;
        if (err.code !== 16000068 || attempt === 9) {
          return Promise.reject(err);
        }
        hilog.info(DOMAIN, 'DeskflowBg', 'waiting for old carrier to stop, retry=%{public}d', attempt + 1);
        await new Promise<void>((resolve) => {
          setTimeout(() => resolve(), 200);
        });
      }
    }
  }

  /** 通知后台承载 Ability 停止（终止旧连接进程） */
  private requestBgStop(): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const opts: commonEventManager.CommonEventPublishData = { data: 'stop', bundleName: this.appBundleName() };
      commonEventManager.publish(BGEVT_STOP, opts, (err) => {
        if (err && err.code) {
          reject(err);
        } else {
          resolve();
        }
      });
    });
  }

  build() {
    Scroll() {
      Column({ space: 12 }) {
        Column({ space: 4 }) {
          Text('Deskflow')
            .fontSize(24)
            .fontWeight(FontWeight.Bold)
          Text('鸿蒙电脑键鼠共享客户端')
            .fontSize(13)
            .fontColor('#888888')
        }
        .width('100%')
        .alignItems(HorizontalAlign.Center)
        .padding({ top: 16, bottom: 8 })

        this.StatusCard()
        this.ConnConfig()
        this.Settings()

        Text('将鼠标移到服务端屏幕右缘进入本机；本机屏幕左缘回到服务端。')
          .fontSize(12)
          .fontColor('#999999')
          .width('100%')
          .textAlign(TextAlign.Center)
          .margin({ top: 8 })
      }
      .width('94%')
      .margin({ left: '3%', right: '3%' })
      .padding({ bottom: 40 })
    }
    .width('100%')
    .height('100%')
    .scrollable(ScrollDirection.Vertical)
    .scrollBar(BarState.Auto)
  }
}
