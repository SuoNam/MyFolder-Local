<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { api, type TrustedDevice } from '../api'
import { authStatus, authTime } from '../lib/auth'

const devices = ref<TrustedDevice[]>([]), available = ref(false), busy = ref(false)
const secureTransport = ref(false)
const name = ref('我的浏览器'), error = ref(''), notice = ref(''), revoking = ref<TrustedDevice | null>(null)
async function refresh() {
  error.value = ''
  try { const result = await api.trustedDevices(); devices.value = result.devices; available.value = result.trusted_login; secureTransport.value = result.secure_transport }
  catch (e) { error.value = e instanceof Error ? e.message : '读取失败' }
}
async function request() {
  busy.value = true; notice.value = ''; error.value = ''
  try { const result = await api.requestTrustedDevice(name.value.trim()); notice.value = result.status === 'approved' ? '此浏览器已经是可信设备' : '申请已提交，请等待管理员审核'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '申请失败' }
  finally { busy.value = false }
}
async function revoke() {
  if (!revoking.value) return
  busy.value = true
  try { await api.revokeTrustedDevice(revoking.value.id); revoking.value = null; notice.value = '免密资格已撤销，关联的免密会话同时失效'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '撤销失败' }
  finally { busy.value = false }
}
onMounted(refresh)
</script>

<template>
  <header class="topbar"><span class="eyebrow">Trusted browsers</span><div class="spacer" /><button class="btn" :disabled="busy" @click="refresh">刷新</button></header>
  <div class="page lan-admin">
    <div class="page-head"><div><h1 class="h1">登录设备</h1><p class="sub">管理员批准后，同一浏览器可免密进入；不会采集或依赖 MAC 地址。</p></div></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }}<button aria-label="关闭错误提示" @click="error = ''">×</button></p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}<button aria-label="关闭成功提示" @click="notice = ''">×</button></p>
    <section class="card"><div class="card-head"><h2 class="h3">申请当前浏览器免密</h2></div>
      <form class="card-pad lan-device-request" @submit.prevent="request">
        <div><label class="label" for="trusted-device-name">设备名称</label><div class="field"><input id="trusted-device-name" v-model="name" required maxlength="40" placeholder="例如：办公室电脑的 Chrome" /></div></div>
        <button class="btn btn-primary" type="submit" :disabled="busy || !available">申请免密登录</button>
      </form>
      <p class="sub lan-trust-help">{{ available ? '仅在个人设备上启用。普通用户由管理员审核，管理员设备由超级管理员审核。' : '管理员已开启仅 HTTPS 免密登录；请通过可信 HTTPS 入口申请，普通账号密码登录不受影响。' }}</p>
      <p v-if="available && !secureTransport" class="sub lan-trust-help">当前使用 HTTP，通信未加密；请仅在可信内网使用，HTTPS 可选。</p>
    </section>
    <section class="card lan-review-card"><div class="card-head"><h2 class="h3">我的登录设备</h2></div>
      <div class="lan-table-wrap"><table class="files"><thead><tr><th>设备</th><th>状态</th><th>最近使用</th><th>到期时间</th><th>操作</th></tr></thead><tbody>
        <tr v-for="device in devices" :key="device.id"><td data-label="设备">{{ device.name }} <span v-if="device.current" class="chip chip-ok">此浏览器</span></td><td data-label="状态">{{ authStatus[device.status] }}</td><td data-label="最近使用">{{ authTime(device.last_used_at) }}</td><td data-label="到期时间">{{ authTime(device.expires_at) }}</td><td data-label="操作"><button v-if="['pending', 'approved'].includes(device.status)" class="btn btn-sm btn-danger" @click="revoking = device">撤销免密</button></td></tr>
        <tr v-if="!devices.length"><td colspan="5" class="lan-table-empty">尚未申请可信浏览器</td></tr>
      </tbody></table></div>
    </section>
    <p class="sub lan-trust-help">免密资格最长 90 天，连续 30 天未使用将过期。清除 Cookie、换浏览器需重新申请；退出登录会撤销当前浏览器资格，重置密码会撤销全部设备。</p>
  </div>
  <div v-if="revoking" class="scrim" @click.self="revoking = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="撤销免密"><div class="sheet-head"><h2 class="h3">撤销 {{ revoking.name }} 的免密资格？</h2></div><div class="sheet-body"><p class="sub">这台浏览器的免密登录会话将立即失效。若是当前浏览器，下次操作可能需要重新输入密码。</p></div><div class="sheet-foot"><button class="btn" @click="revoking = null">取消</button><div class="spacer" /><button class="btn btn-danger" :disabled="busy" @click="revoke">确认撤销</button></div></section></div>
</template>
