<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { api, type ServerInfo, type User } from './api'
import Icon from './components/Icon.vue'
import SelectMenu from './components/SelectMenu.vue'
import LoginView from './views/LoginView.vue'
import FilesView from './views/FilesView.vue'
import AdminView from './views/AdminView.vue'
import InfoView from './views/InfoView.vue'
import RemotesView from './views/RemotesView.vue'
import PasswordView from './views/PasswordView.vue'

type Page = 'files' | 'admin' | 'info' | 'remotes' | 'password'
const user = ref<User | null>(null)
const info = ref<ServerInfo | null>(null)
const active = ref<Page>('files')
const serverChoice = ref<string | number>('local')
const loading = ref(true)
const logoutError = ref('')
const remoteId = computed(() => typeof serverChoice.value === 'number' ? serverChoice.value : null)
const serverName = computed(() => remoteId.value == null ? (info.value?.name || '本服务器')
  : info.value?.remotes.find(item => item.id === remoteId.value)?.remote_name || '外连服务器')
const serverOptions = computed(() => [
  { value: 'local', label: info.value?.name || '本服务器' },
  ...(info.value?.remotes.filter(item => item.status === 'approved').map(item => ({ value: item.id, label: item.remote_name })) || []),
])
const roleLabel = (role: User['role']) => ({ superadmin: '超级管理员', admin: '管理员', user: '普通用户' })[role]

async function refreshInfo() {
  try { info.value = await api.serverInfo() } catch { /* file area can still load */ }
}
onMounted(async () => {
  try {
    user.value = await api.me()
    active.value = user.value.reset_until > Date.now() / 1000 ? 'password' : 'files'
    await refreshInfo()
  } catch { user.value = null }
  finally { loading.value = false }
})

function signedIn(next: User) {
  user.value = next
  active.value = next.reset_until > Date.now() / 1000 ? 'password' : 'files'
  void refreshInfo()
}
function selectServer(value: string | number) {
  serverChoice.value = value
  active.value = 'files'
}
async function logout() {
  try { await api.logout() }
  catch (error) { logoutError.value = error instanceof Error ? error.message : '退出失败'; return }
  user.value = null
  info.value = null
  active.value = 'files'
  serverChoice.value = 'local'
}
</script>

<template>
  <div v-if="loading" class="startup"><div class="led is-busy" />正在连接内网服务…</div>
  <LoginView v-else-if="!user" @signed-in="signedIn" />
  <div v-else class="shell lan-shell">
    <nav class="rail" aria-label="主导航">
      <div class="brand"><img class="brand-mark" src="/myfolder-icon-v2.png" alt="" /><div class="brand-name">MyFolder</div><div class="brand-ver">LAN 1.5</div></div>
      <div class="nav-group"><div class="nav-head eyebrow">Workspace</div><div class="nav">
        <button type="button" class="nav-item" :class="{ 'is-active': active === 'files' }" @click="active = 'files'"><Icon name="folder" /><span>文件区</span></button>
        <div class="lan-server-picker"><label class="label" for="server-choice">当前服务器</label><SelectMenu id="server-choice" :model-value="serverChoice" :options="serverOptions" @update:model-value="selectServer" /></div>
        <button type="button" class="nav-item" :class="{ 'is-active': active === 'info' }" @click="active = 'info'"><Icon name="server" /><span>服务器信息</span></button>
        <button v-if="user.role !== 'user'" type="button" class="nav-item" :class="{ 'is-active': active === 'admin' }" @click="active = 'admin'"><Icon name="shield" /><span>用户与权限</span></button>
        <button v-if="user.role === 'superadmin'" type="button" class="nav-item" :class="{ 'is-active': active === 'remotes' }" @click="active = 'remotes'"><Icon name="devices" /><span>外连服务器</span></button>
        <button v-if="user.reset_until > Date.now() / 1000" type="button" class="nav-item" :class="{ 'is-active': active === 'password' }" @click="active = 'password'"><Icon name="lock" /><span>修改密码</span></button>
      </div></div>
      <div class="rail-foot"><div class="me"><div class="me-av">{{ user.username.slice(0, 2).toUpperCase() }}</div><div class="me-info"><div class="me-name">{{ user.username }}</div><div class="me-sub"><span class="led is-on" />{{ roleLabel(user.role) }} · 内网已连接</div></div></div><p v-if="logoutError" class="sub is-error">{{ logoutError }}</p><button type="button" class="btn btn-quiet rail-logout" @click="logout"><Icon name="logout" />退出登录</button></div>
    </nav>
    <div class="main">
      <FilesView v-if="active === 'files'" :key="`${user.id}-${serverChoice}`" :remote-id="remoteId" :server-name="serverName" />
      <AdminView v-else-if="active === 'admin' && user.role !== 'user'" :current-user="user" />
      <InfoView v-else-if="active === 'info'" :role="user.role" @updated="info = $event" />
      <RemotesView v-else-if="active === 'remotes' && user.role === 'superadmin'" @updated="refreshInfo" />
      <PasswordView v-else-if="active === 'password'" :user="user" @changed="user = $event; active = 'files'" />
    </div>
  </div>
</template>
