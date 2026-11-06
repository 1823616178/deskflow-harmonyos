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

// NOTE: the old NDKInputEventInterceptor sample buttons ('Add key event
// interceptor' etc.) no longer exist in Index.ets. These tests now assert
// the current Deskflow UI (title, connection section, clipboard toggle,
// Connect/Disconnect buttons). Interceptor diagnostics are NAPI-only;
// see docs/ARCHITECTURE.md §3.3.
import AbilityDelegatorRegistry from '@ohos.app.ability.abilityDelegatorRegistry';
import { describe, it, expect } from '@ohos/hypium';
import { Driver, ON } from '@ohos.UiTest';
import { hilog } from '@kit.PerformanceAnalysisKit';

const TAG = '[Deskflow]';
const DOMAIN = 0xF811;
const BUNDLE = 'Deskflow_';

export default function abilityTest() {
  describe('ActsAbilityTest', () => {
    /**
     * 打开应用
     */
    it('NDKInputEventInterceptorAPI_StartAbility_001', 0, async (done: Function) => {
      hilog.info(DOMAIN, TAG, BUNDLE + 'StartAbility_001, begin');
      let driver = Driver.create();
      let abilityDelegator = AbilityDelegatorRegistry.getAbilityDelegator();
      try {
        await abilityDelegator.startAbility({
          bundleName: 'com.deskflow.ohospoc',
          abilityName: 'EntryAbility'
        });
      } catch (exception) {
        hilog.info(DOMAIN, TAG, BUNDLE + `StartAbility_001 exception = ${JSON.stringify(exception)}`);
        expect().assertFail();
      }
      await driver.delayMs(3000);
      await driver.assertComponentExist(ON.text('Deskflow'));
      await driver.assertComponentExist(ON.text('服务端连接'));
      await driver.assertComponentExist(ON.text('剪贴板同步'));
      await driver.assertComponentExist(ON.text('连接'));
      done();
      hilog.info(DOMAIN, TAG, BUNDLE + 'StartAbility_001 end')
    })

    /**
     * 添加按键事件拦截
     */
    it('Deskflow_Connect_001', 0, async (done: Function) => {
      hilog.info(DOMAIN, TAG, BUNDLE + 'addKeyEventInterceptor_001, begin');
      let driver = Driver.create();
      let abilityDelegator = AbilityDelegatorRegistry.getAbilityDelegator();
      try {
        await abilityDelegator.startAbility({
          bundleName: 'com.deskflow.ohospoc',
          abilityName: 'EntryAbility'
        });
      } catch (exception) {
        hilog.info(DOMAIN, TAG, BUNDLE + `addKeyEventInterceptor_001 exception = ${JSON.stringify(exception)}`);
        expect().assertFail();
      }
      await driver.delayMs(3000);
      await driver.assertComponentExist(ON.text('Deskflow'));
      let button = await driver.findComponent(ON.text('连接'));
      await button.click();
      await driver.delayMs(3000);
      done();
      hilog.info(DOMAIN, TAG, BUNDLE + 'addKeyEventInterceptor_001 end')
    })

    /**
     * 移除按键事件拦截
     */
    it('Deskflow_Disconnect_001', 0, async (done: Function) => {
      hilog.info(DOMAIN, TAG, BUNDLE + 'removeKeyEventInterceptor_001, begin');
      let driver = Driver.create();
      let abilityDelegator = AbilityDelegatorRegistry.getAbilityDelegator();
      try {
        await abilityDelegator.startAbility({
          bundleName: 'com.deskflow.ohospoc',
          abilityName: 'EntryAbility'
        });
      } catch (exception) {
        hilog.info(DOMAIN, TAG, BUNDLE + `removeKeyEventInterceptor_001 exception = ${JSON.stringify(exception)}`);
        expect().assertFail();
      }
      await driver.delayMs(3000);
      await driver.assertComponentExist(ON.text('服务端连接'));
      let button = await driver.findComponent(ON.text('断开'));
      await button.click();
      await driver.delayMs(3000);
      done();
      hilog.info(DOMAIN, TAG, BUNDLE + 'removeKeyEventInterceptor_001 end')
    })

    /**
     * 添加输入事件拦截
     */
    it('Deskflow_Connect_002', 0, async (done: Function) => {
      hilog.info(DOMAIN, TAG, BUNDLE + 'addEventInterceptor_001, begin');
      let driver = Driver.create();
      let abilityDelegator = AbilityDelegatorRegistry.getAbilityDelegator();
      try {
        await abilityDelegator.startAbility({
          bundleName: 'com.deskflow.ohospoc',
          abilityName: 'EntryAbility'
        });
      } catch (exception) {
        hilog.info(DOMAIN, TAG, BUNDLE + `addEventInterceptor_001 exception = ${JSON.stringify(exception)}`);
        expect().assertFail();
      }
      await driver.delayMs(3000);
      await driver.assertComponentExist(ON.text('剪贴板同步'));
      let button = await driver.findComponent(ON.text('连接'));
      await button.click();
      await driver.delayMs(3000);
      done();
      hilog.info(DOMAIN, TAG, BUNDLE + 'addEventInterceptor_001 end')
    })

    /**
     * 添加输入事件拦截
     */
    it('Deskflow_Disconnect_002', 0, async (done: Function) => {
      hilog.info(DOMAIN, TAG, BUNDLE + 'removeEventInterceptor_001, begin');
      let driver = Driver.create();
      let abilityDelegator = AbilityDelegatorRegistry.getAbilityDelegator();
      try {
        await abilityDelegator.startAbility({
          bundleName: 'com.deskflow.ohospoc',
          abilityName: 'EntryAbility'
        });
      } catch (exception) {
        hilog.info(DOMAIN, TAG, BUNDLE + `removeEventInterceptor_001 exception = ${JSON.stringify(exception)}`);
        expect().assertFail();
      }
      await driver.delayMs(3000);
      await driver.assertComponentExist(ON.text('连接'));
      let button = await driver.findComponent(ON.text('断开'));
      await button.click();
      await driver.delayMs(3000);
      done();
      hilog.info(DOMAIN, TAG, BUNDLE + 'removeEventInterceptor_001 end')
    })
  })
}