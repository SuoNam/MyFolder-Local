<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, type RegistrationStatus, type User } from '../api'
import Icon from '../components/Icon.vue'

const emit = defineEmits<{ signedIn: [user: User] }>()
const mode = ref<'login' | 'register' | 'forgot'>('login')
const username = ref(''), password = ref(''), confirmation = ref('')
const deviceName = ref('我的浏览器'), trustDevice = ref(false), trustAvailable = ref(false)
const secureTransport = ref(false)
const busy = ref(false), error = ref(''), notice = ref('')
const registration = ref<RegistrationStatus | null>(null)
const title = computed(() => ({ login: '登录 MyFolder', register: '申请注册', forgot: '忘记密码' })[mode.value])
let timer = 0
let mounted = false

function switchMode(next: typeof mode.value) {
  mode.value = next; error.value = ''; notice.value = ''; password.value = ''; confirmation.value = ''
}
async function refreshRegistration() {
  try {
    registration.value = await api.registrationStatus()
    if (mounted && registration.value.status === 'approved' && !registration.value.claimed) await activate()
  }
  catch { /* No receipt or temporarily offline; never infer approval. */ }
}
async function activate() {
  if (busy.value || !mounted) return
  busy.value = true; error.value = ''
  try { emit('signedIn', await api.activateRegistration()) }
  catch (e) { error.value = e instanceof Error ? e.message : '进入账号失败' }
  finally { busy.value = false }
}
async function submit() {
  if (busy.value) return
  error.value = ''; notice.value = ''; busy.value = true
  try {
    if (mode.value === 'forgot') notice.value = (await api.requestPassword(username.value.trim())).message
    else if (mode.value === 'register') {
      if (password.value !== confirmation.value) throw new Error('两次输入的密码不一致')
      registration.value = await api.register(username.value.trim(), password.value, deviceName.value.trim(), trustDevice.value)
      password.value = ''; confirmation.value = ''
      notice.value = '申请已提交。审核通过前无法登录或访问文件。'
    } else emit('signedIn', await api.login(username.value.trim(), password.value))
  } catch (e) { error.value = e instanceof Error ? e.message : '操作失败' }
  finally { busy.value = false }
}
onMounted(async () => {
  mounted = true
  try { const options = await api.authOptions(); trustAvailable.value = options.trusted_login; secureTransport.value = options.secure_transport }
  catch { /* Password login remains available. */ }
  await refreshRegistration()
  if (!mounted) return
  timer = window.setInterval(() => {
    if (document.visibilityState === 'visible' && registration.value?.status === 'pending') void refreshRegistration()
  }, 10_000)
})
onUnmounted(() => { mounted = false; clearInterval(timer) })
</script>

<template>
  <div class="auth">
    <section class="auth-form">
      <div class="auth-inner lan-auth-inner">
        <div class="brand auth-brand"><img class="brand-mark" src="/myfolder-icon-v2.png" alt="" /><div class="brand-name">MyFolder</div><div class="brand-ver">LAN 1.5</div></div>
        <p class="eyebrow">Private workspace</p>
        <h1 class="h1 auth-title">{{ title }}</h1>
        <p class="sub auth-sub">{{ mode === 'forgot' ? '提交申请，由上级管理员审核。通过后使用临时密码 123456 登录，并在 30 分钟内修改。' : mode === 'register' ? '注册普通账号，由管理员审核；文件权限由管理员另行分配。' : '继续管理内网文件与访问权限。' }}</p>
        <section v-if="registration" class="card lan-registration-status" aria-label="注册审核状态">
          <strong>{{ registration.username }} · {{ ({ pending: '等待管理员审核', approved: '注册已通过', rejected: '申请已驳回', expired: '申请已过期' })[registration.status] }}</strong>
          <p class="sub">{{ registration.status === 'pending' ? '此浏览器会自动检查审核结果，也可以手动刷新。' : registration.status === 'approved' ? '可使用注册时设置的密码登录；如未领取，可直接进入。' : '请联系管理员了解原因，或重新提交申请。' }}</p>
          <button v-if="registration.status === 'approved' && !registration.claimed" class="btn btn-primary" :disabled="busy" @click="activate">审核通过，进入账号</button>
          <button v-else-if="registration.status === 'pending'" class="btn" type="button" @click="refreshRegistration">检查审核结果</button>
        </section>
        <form @submit.prevent="submit">
          <label class="label" for="acct">账号</label>
          <div class="field"><Icon name="user" /><input id="acct" v-model="username" autocomplete="username" :minlength="mode === 'register' ? 3 : undefined" maxlength="64" autofocus required /></div>
          <template v-if="mode !== 'forgot'">
            <label class="label field-gap" for="pwd">{{ mode === 'register' ? '密码（至少 8 位）' : '密码' }}</label>
            <div class="field"><Icon name="lock" /><input id="pwd" v-model="password" type="password" :autocomplete="mode === 'register' ? 'new-password' : 'current-password'" :minlength="mode === 'register' ? 8 : undefined" maxlength="1024" required /></div>
          </template>
          <template v-if="mode === 'register'">
            <label class="label field-gap" for="confirm-password">确认密码</label>
            <div class="field"><input id="confirm-password" v-model="confirmation" type="password" autocomplete="new-password" minlength="8" maxlength="1024" required /></div>
            <label class="label field-gap" for="registration-device-name">申请设备名称</label>
            <div class="field"><input id="registration-device-name" v-model="deviceName" maxlength="40" required placeholder="例如：办公室电脑的 Chrome" /></div>
            <label class="lan-trust-choice"><input v-model="trustDevice" type="checkbox" :disabled="!trustAvailable" />同时申请此浏览器免密登录</label>
            <p class="sub">{{ trustAvailable ? '管理员批准后，此浏览器可自动登录；仅在个人设备上启用。' : '管理员已开启仅 HTTPS 免密登录；当前仍可申请注册并使用密码登录。' }}</p>
            <p v-if="trustAvailable && !secureTransport" class="sub">当前使用 HTTP，通信未加密；请仅在可信内网使用，HTTPS 可选。</p>
          </template>
          <p v-if="error" class="form-message is-error" role="alert">{{ error }}</p>
          <p v-if="notice" class="form-message is-ok" role="status">{{ notice }}</p>
          <button class="btn btn-primary submit-button" :disabled="busy || (mode === 'register' && registration?.status === 'pending')">{{ busy ? '请稍候…' : mode === 'forgot' ? '提交申请' : mode === 'register' ? '提交注册申请' : '登录' }}</button>
        </form>
        <div v-if="mode === 'login'" class="lan-auth-links"><button type="button" class="btn btn-quiet" @click="switchMode('forgot')">忘记密码？</button><button type="button" class="btn btn-quiet" @click="switchMode('register')">申请注册</button></div>
        <button v-else type="button" class="btn btn-quiet lan-auth-switch" @click="switchMode('login')">返回登录</button>
      </div>
    </section>
    <section class="auth-art">
      <div class="auth-art-copy"><div class="eyebrow">Files · permission · local</div><h2 class="h1">Myfolder</h2><p class="sub">服务个人部署版本</p></div>
      <div class="session-card"><span class="eyebrow">Local workspace</span><strong>注册审核 · 可信浏览器免密</strong><strong>路径授权 · 灵活管理</strong><p class="sub">文件共享与访问权限，由你掌控。</p></div>
    </section>
  </div>
</template>
