<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { api, type ServerInfo, type User } from '../api'
import Icon from '../components/Icon.vue'

const props = defineProps<{ role: User['role'] }>()
const emit = defineEmits<{ updated: [info: ServerInfo] }>()
const info = ref<ServerInfo | null>(null)
const name = ref('')
const limitGb = ref(5)
const busy = ref(false)
const error = ref('')
const notice = ref('')
const formatBytes = (bytes: number) => `${(bytes / 1024 ** 3).toFixed(2)} GB`

async function refresh() {
  error.value = ''
  try {
    info.value = await api.serverInfo()
    name.value = info.value.name
    limitGb.value = Number((info.value.storage_limit / 1024 ** 3).toFixed(2))
    emit('updated', info.value)
  } catch (e) { error.value = e instanceof Error ? e.message : '读取服务器信息失败' }
}
onMounted(() => void refresh())

async function save() {
  if (busy.value) return
  busy.value = true
  error.value = ''
  notice.value = ''
  try {
    const bytes = Math.round(limitGb.value * 1024 ** 3)
    info.value = await api.saveServerInfo(name.value.trim(), bytes)
    emit('updated', info.value)
    notice.value = '服务器配置已保存'
  } catch (e) { error.value = e instanceof Error ? e.message : '保存失败' }
  finally { busy.value = false }
}
</script>

<template>
  <header class="topbar"><span class="eyebrow">Server / Overview</span><div class="spacer" /><button class="btn" @click="refresh"><Icon name="refresh" />刷新</button></header>
  <div class="page lan-info">
    <div class="page-head"><div><h1 class="h1">服务器信息</h1><p class="sub">查看当前服务器的存储与外连状态。</p></div></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }}</p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}</p>
    <template v-if="info">
      <div class="lan-stat-grid">
        <div class="card lan-stat"><span class="eyebrow">Server name</span><strong>{{ info.name }}</strong><span class="sub">当前服务器</span></div>
        <div class="card lan-stat"><span class="eyebrow">Storage used</span><strong>{{ formatBytes(info.storage_used) }}</strong><span class="sub">上限 {{ formatBytes(info.storage_limit) }}</span></div>
        <div class="card lan-stat"><span class="eyebrow">Files</span><strong>{{ info.file_count }}</strong><span class="sub">存储路径中的文件总数</span></div>
        <div class="card lan-stat"><span class="eyebrow">Connections</span><strong>{{ info.remotes.filter(item => item.status === 'approved').length }}</strong><span class="sub">已连接的外部服务器</span></div>
      </div>
      <section class="card lan-info-card"><div class="card-head"><Icon name="server" /><h2 class="h3">存储与连接</h2></div><div class="card-pad lan-info-list"><div><span class="sub">文件存储路径</span><strong class="mono">{{ info.storage_path }}</strong></div><div><span class="sub">存储使用率</span><div class="prog"><span :style="{ width: `${Math.min(100, info.storage_used / info.storage_limit * 100)}%` }" /></div></div><div><span class="sub">外连服务器</span><strong>{{ info.remotes.filter(item => item.status === 'approved').map(item => item.remote_name).join('、') || '暂无' }}</strong></div></div></section>
      <section v-if="props.role === 'superadmin'" class="card lan-info-card"><div class="card-head"><Icon name="settings" /><div><h2 class="h3">服务器配置</h2><p class="sub">仅超级管理员可以修改。</p></div></div><form class="card-pad lan-settings-form" @submit.prevent="save"><div><label class="label" for="server-name">服务器名称</label><div class="field"><input id="server-name" v-model="name" maxlength="80" required /></div></div><div><label class="label" for="storage-limit">存储上限（GB）</label><div class="field"><input id="storage-limit" v-model.number="limitGb" type="number" min="0.01" max="1048576" step="0.01" required /></div></div><button class="btn btn-primary" :disabled="busy">保存配置</button></form></section>
    </template>
  </div>
</template>
