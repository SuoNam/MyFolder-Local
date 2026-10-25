<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from 'vue'
import { api, type ServerInfo, type User } from './api'
import Icon from './components/Icon.vue'
import SelectMenu from './components/SelectMenu.vue'
import LoginView from './views/LoginView.vue'
import FilesView from './views/FilesView.vue'
import AdminView from './views/AdminView.vue'
import InfoView from './views/InfoView.vue'
import RemotesView from './views/RemotesView.vue'
import PasswordView from './views/PasswordView.vue'
import TrustedDevicesView from './views/TrustedDevicesView.vue'

type Page = 'files' | 'admin' | 'info' | 'remotes' | 'password' | 'trusted'
const user = ref<User | null>(null)
const info = ref<ServerInfo | null>(null)
const active = ref<Page>('files')
const serverChoice = ref<string | number>('local')
const loading = ref(true)
const logoutError = ref('')
const mobileQuery = window.matchMedia('(max-width: 760px)')
const isMobile = ref(mobileQuery.matches)
const navigationOpen = ref(false)
const navigation = ref<HTMLElement | null>(null)
const menuToggle = ref<HTMLButtonElement | null>(null)
const menuClose = ref<HTMLButtonElement | null>(null)
let previousOverflow = ''
const pageLabel = computed(() => ({ files: '文件区', info: '服务器信息', admin: '用户与权限', remotes: '外连服务器', password: '修改密码', trusted: '登录设备' })[active.value])
const remoteId = computed(() => typeof serverChoice.value === 'number' ? serverChoice.value : null)
const serverName = computed(() => remoteId.value == null ? (info.value?.name || '本服务器')
  : info.value?.remotes.find(item => item.id === remoteId.value)?.remote_name || '外连服务器')
const serverOptions = computed(() => [
  { value: 'local', label: info.value?.name || '本服务器' },
  ...(info.value?.remotes.filter(item => item.status === 'approved').map(item => ({ value: item.id, label: item.remote_name })) || []),
])
const roleLabel = (role: User['role']) => ({ superadmin: '超级管理员', admin: '管理员', user: '普通用户' })[role]

async function openNavigation() {
  previousOverflow = document.body.style.overflow
  document.body.style.overflow = 'hidden'
  navigationOpen.value = true
  await nextTick()
  requestAnimationFrame(() => menuClose.value?.focus({ preventScroll: true }))
}
function closeNavigation(restoreFocus = true) {
  if (!navigationOpen.value) return
  navigationOpen.value = false
  document.body.style.overflow = previousOverflow
  if (restoreFocus && isMobile.value) void nextTick(() => menuToggle.value?.focus({ preventScroll: true }))
}
function navigate(page: Page) {
  active.value = page
  closeNavigation()
  void nextTick(() => window.scrollTo(0, 0))
}
function onViewportChange(event: MediaQueryListEvent) {
  isMobile.value = event.matches
  closeNavigation(false)
}
function onNavigationKeydown(event: KeyboardEvent) {
  if (event.defaultPrevented || !isMobile.value || !navigationOpen.value) return
  if (event.key === 'Escape') { event.preventDefault(); closeNavigation(); return }
  if (event.key !== 'Tab') return
  const targets = Array.from(navigation.value?.querySelectorAll<HTMLElement>('button:not(:disabled), a[href], input:not(:disabled), [tabindex="0"]') || [])
    .filter(element => element.getClientRects().length > 0)
  const first = targets[0], last = targets[targets.length - 1]
  if (event.shiftKey && document.activeElement === first) { event.preventDefault(); last?.focus() }
  else if (!event.shiftKey && document.activeElement === last) { event.preventDefault(); first?.focus() }
}
function sessionExpired() {
  if (!user.value) return
  user.value = null; info.value = null; active.value = 'files'; serverChoice.value = 'local'
  closeNavigation(false)
}
onMounted(() => {
  mobileQuery.addEventListener('change', onViewportChange)
  window.addEventListener('myfolder-session-expired', sessionExpired)
})
onBeforeUnmount(() => {
  mobileQuery.removeEventListener('change', onViewportChange)
  window.removeEventListener('myfolder-session-expired', sessionExpired)
  closeNavigation(false)
})

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
  closeNavigation()
  void nextTick(() => window.scrollTo(0, 0))
}
async function logout() {
  try { await api.logout() }
  catch (error) { logoutError.value = error instanceof Error ? error.message : '退出失败'; return }
  user.value = null
  info.value = null
  active.value = 'files'
  serverChoice.value = 'local'
  closeNavigation(false)
}
</script>

<template>
  <div v-if="loading" class="startup"><div class="led is-busy" />正在连接内网服务…</div>
  <LoginView v-else-if="!user" @signed-in="signedIn" />
  <div v-else class="shell lan-shell">
    <header class="mobile-appbar" :inert="navigationOpen">
      <button ref="menuToggle" type="button" class="btn mobile-menu-toggle" aria-label="打开导航菜单" aria-controls="main-navigation" :aria-expanded="navigationOpen" @click="openNavigation">
        <svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" aria-hidden="true"><path d="M4 6h16M4 12h16M4 18h16" /></svg>
      </button>
      <img class="brand-mark" src="/myfolder-icon-v2.png" alt="" />
      <div class="mobile-appbar-title"><strong>MyFolder</strong><span>{{ pageLabel }}</span></div>
      <span class="mobile-user-role">{{ roleLabel(user.role) }}</span>
    </header>
    <div v-if="isMobile && navigationOpen" class="navigation-backdrop" aria-hidden="true" @click="closeNavigation()" />
    <nav id="main-navigation" ref="navigation" class="rail" :class="{ 'is-open': navigationOpen }" :role="isMobile ? 'dialog' : undefined" :aria-modal="isMobile && navigationOpen ? true : undefined" :aria-hidden="isMobile && !navigationOpen ? true : undefined" :inert="isMobile && !navigationOpen" aria-label="主导航" @keydown="onNavigationKeydown">
      <div class="brand"><img class="brand-mark" src="/myfolder-icon-v2.png" alt="" /><div class="brand-name">MyFolder</div><div class="brand-ver">LAN 1.5</div></div>
      <button ref="menuClose" type="button" class="btn mobile-navigation-close" aria-label="关闭导航菜单" @click="closeNavigation()">关闭 <span aria-hidden="true">×</span></button>
      <div class="nav-group"><div class="nav-head eyebrow">Workspace</div><div class="nav">
        <button type="button" class="nav-item" :class="{ 'is-active': active === 'files' }" @click="navigate('files')"><Icon name="folder" /><span>文件区</span></button>
        <div class="lan-server-picker"><label class="label" for="server-choice">当前服务器</label><SelectMenu id="server-choice" :model-value="serverChoice" :options="serverOptions" @update:model-value="selectServer" /></div>
        <button type="button" class="nav-item" :class="{ 'is-active': active === 'info' }" @click="navigate('info')"><Icon name="server" /><span>服务器信息</span></button>
        <button type="button" class="nav-item" :class="{ 'is-active': active === 'trusted' }" @click="navigate('trusted')"><Icon name="lock" /><span>登录设备</span></button>
        <button v-if="user.role !== 'user'" type="button" class="nav-item" :class="{ 'is-active': active === 'admin' }" @click="navigate('admin')"><Icon name="shield" /><span>用户与权限</span></button>
        <button v-if="user.role === 'superadmin'" type="button" class="nav-item" :class="{ 'is-active': active === 'remotes' }" @click="navigate('remotes')"><Icon name="devices" /><span>外连服务器</span></button>
        <button v-if="user.reset_until > Date.now() / 1000" type="button" class="nav-item" :class="{ 'is-active': active === 'password' }" @click="navigate('password')"><Icon name="lock" /><span>修改密码</span></button>
      </div></div>
      <div class="rail-foot"><div class="me"><div class="me-av">{{ user.username.slice(0, 2).toUpperCase() }}</div><div class="me-info"><div class="me-name">{{ user.username }}</div><div class="me-sub"><span class="led is-on" />{{ roleLabel(user.role) }} · 内网已连接</div></div></div><p v-if="logoutError" class="sub is-error">{{ logoutError }}</p><button type="button" class="btn btn-quiet rail-logout" @click="logout"><Icon name="logout" />退出登录</button></div>
    </nav>
    <div class="main" :inert="isMobile && navigationOpen">
      <FilesView v-if="active === 'files'" :key="`${user.id}-${serverChoice}`" :remote-id="remoteId" :server-name="serverName" />
      <AdminView v-else-if="active === 'admin' && user.role !== 'user'" :current-user="user" />
      <InfoView v-else-if="active === 'info'" :role="user.role" @updated="info = $event" />
      <TrustedDevicesView v-else-if="active === 'trusted'" />
      <RemotesView v-else-if="active === 'remotes' && user.role === 'superadmin'" @updated="refreshInfo" />
      <PasswordView v-else-if="active === 'password'" :user="user" @changed="user = $event; active = 'files'" />
    </div>
  </div>
</template>
