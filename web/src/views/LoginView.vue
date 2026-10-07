<script setup lang="ts">
import { ref } from 'vue'
import { api, type User } from '../api'
import Icon from '../components/Icon.vue'

const emit = defineEmits<{ signedIn: [user: User] }>()
const username = ref('')
const password = ref('')
const busy = ref(false)
const error = ref('')
const forgot = ref(false)
const forgotNotice = ref('')

async function submit() {
  if (busy.value) return
  error.value = ''
  busy.value = true
  try { emit('signedIn', await api.login(username.value.trim(), password.value)) }
  catch (e) { error.value = e instanceof Error ? e.message : '登录失败' }
  finally { busy.value = false }
}

async function requestReset() {
  if (busy.value) return
  error.value = ''
  busy.value = true
  try { forgotNotice.value = (await api.requestPassword(username.value.trim())).message }
  catch (e) { error.value = e instanceof Error ? e.message : '申请失败' }
  finally { busy.value = false }
}
</script>

<template>
  <div class="auth">
    <section class="auth-form">
      <div class="auth-inner lan-auth-inner">
        <div class="brand auth-brand"><img class="brand-mark" src="/myfolder-icon-v2.png" alt="" /><div class="brand-name">MyFolder</div><div class="brand-ver">LAN 1.5</div></div>
        <p class="eyebrow">Private workspace</p>
        <h1 class="h1 auth-title">{{ forgot ? '忘记密码' : '登录 MyFolder' }}</h1>
        <p class="sub auth-sub">{{ forgot ? '提交申请，由上级管理员审核。通过后使用临时密码 123456 登录，并在 30 分钟内修改。' : '继续管理内网文件与访问权限。' }}</p>
        <form @submit.prevent="forgot ? requestReset() : submit()">
          <label class="label" for="acct">账号</label>
          <div class="field"><Icon name="user" /><input id="acct" v-model="username" autocomplete="username" autofocus required /></div>
          <label v-if="!forgot" class="label field-gap" for="pwd">密码</label>
          <div v-if="!forgot" class="field"><Icon name="lock" /><input id="pwd" v-model="password" type="password" autocomplete="current-password" required /></div>
          <p v-if="error" class="form-message is-error" role="alert">{{ error }}</p>
          <p v-if="forgotNotice" class="form-message is-ok" role="status">{{ forgotNotice }}</p>
          <button class="btn btn-primary submit-button" :disabled="busy">{{ busy ? '请稍候…' : forgot ? '提交申请' : '登录' }}</button>
        </form>
        <button type="button" class="btn btn-quiet lan-auth-switch" @click="forgot = !forgot; error = ''; forgotNotice = ''">{{ forgot ? '返回登录' : '忘记密码？' }}</button>
      </div>
    </section>
    <section class="auth-art">
      <div class="auth-art-copy">
        <div class="eyebrow">Files · permission · local</div>
        <h2 class="h1">Myfolder</h2>
        <p class="sub">服务个人部署版本</p>
      </div>
      <div class="session-card">
        <span class="eyebrow">Local workspace</span>
        <strong>内网直连 · 高效传输</strong>
        <strong>路径授权 · 灵活管理</strong>
        <p class="sub">文件共享与访问权限，由你掌控。</p>
      </div>
    </section>
  </div>
</template>
