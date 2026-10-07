<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { api, type Permission, type RemoteIncoming, type RemoteOutgoing } from '../api'
import Icon from '../components/Icon.vue'

const emit = defineEmits<{ updated: [] }>()

const outgoing = ref<RemoteOutgoing[]>([])
const incoming = ref<RemoteIncoming[]>([])
const url = ref('')
const selectedId = ref('')
const grant = reactive({ path: '/', upload: false, download: false, modify: false, delete: false })
const selected = computed(() => incoming.value.find(item => item.id === selectedId.value))
const error = ref('')
const notice = ref('')
const busy = ref(false)
const labels: Record<Permission, string> = { upload: '上传', download: '下载', modify: '修改', delete: '删除' }
const statusLabel = (status: string) => ({ pending: '待审批', approved: '已连接', rejected: '已驳回' })[status as 'pending' | 'approved' | 'rejected'] || status

async function refresh() {
  error.value = ''
  try {
    const [a, b] = await Promise.all([api.remotes(), api.incomingRemotes()])
    outgoing.value = a.outgoing
    incoming.value = b.incoming
    if (!selected.value) selectedId.value = incoming.value[0]?.id || ''
    emit('updated')
  } catch (e) { error.value = e instanceof Error ? e.message : '读取连接信息失败' }
}
onMounted(() => void refresh())

async function add() {
  if (busy.value) return
  busy.value = true
  error.value = ''; notice.value = ''
  try { await api.addRemote(url.value.trim()); url.value = ''; notice.value = '连接申请已发送，等待对方超级管理员审批'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '申请失败' }
  finally { busy.value = false }
}
async function sync() {
  error.value = ''; notice.value = ''
  try {
    const result = await api.syncRemotes()
    notice.value = result.errors.length ? `已检查，${result.errors.join('、')} 暂时无法连接` : '连接状态已更新'
    await refresh()
  } catch (e) { error.value = e instanceof Error ? e.message : '检查失败' }
}
async function saveGrant() {
  if (!selectedId.value) return
  error.value = ''; notice.value = ''
  try { await api.setRemoteGrant({ id: selectedId.value, ...grant }); notice.value = '路径权限已保存'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '保存失败' }
}
async function removeGrant(id: number) {
  error.value = ''
  try { await api.removeRemoteGrant(id); notice.value = '授权已撤销'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '撤销失败' }
}
async function review(id: string, decision: 'approve' | 'reject') {
  error.value = ''
  try { await api.reviewRemote(id, decision); notice.value = decision === 'approve' ? '连接已批准' : '连接已驳回'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '审核失败' }
}
</script>

<template>
  <header class="topbar"><span class="eyebrow">Server / Connections</span><div class="spacer" /><button class="btn" @click="sync"><Icon name="refresh" />检查连接状态</button></header>
  <div class="page lan-remotes">
    <div class="page-head"><div><h1 class="h1">外连服务器</h1><p class="sub">连接另一台 MyFolder 后端，并由对方超级管理员审批路径与操作权限。</p></div></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }}</p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}</p>
    <section class="card lan-info-card"><div class="card-head"><Icon name="server" /><h2 class="h3">发起连接</h2></div><form class="card-pad lan-connect-form" @submit.prevent="add"><div><label class="label" for="remote-url">对方公网 IP 或域名与端口</label><div class="field"><input id="remote-url" v-model="url" required placeholder="https://files.example.com:8443" /></div></div><button class="btn btn-primary" :disabled="busy">发送连接申请</button></form><p class="card-pad sub lan-connect-note">公网连接需要 HTTPS；内网 IP 可以使用 HTTP。对方批准后，点击“检查连接状态”。</p></section>
    <section class="card lan-info-card"><div class="card-head"><Icon name="devices" /><h2 class="h3">已发起的连接</h2></div><div class="lan-table-wrap"><table class="files"><thead><tr><th>服务器</th><th>地址</th><th>状态</th></tr></thead><tbody><tr v-for="item in outgoing" :key="item.id"><td><strong>{{ item.remote_name }}</strong></td><td class="mono">{{ item.url }}</td><td><span class="chip" :class="item.status === 'approved' ? 'chip-ok' : 'chip-idle'">{{ statusLabel(item.status) }}</span></td></tr><tr v-if="!outgoing.length"><td colspan="3" class="lan-table-empty">尚未发起连接</td></tr></tbody></table></div></section>
    <section class="card lan-info-card"><div class="card-head"><Icon name="shield" /><div><h2 class="h3">收到的连接申请</h2><p class="sub">先为申请方开放路径和操作，再批准连接。</p></div></div><div class="lan-table-wrap"><table class="files"><thead><tr><th>申请服务器</th><th>状态</th><th>操作</th></tr></thead><tbody><tr v-for="item in incoming" :key="item.id"><td>{{ item.source_name }}</td><td>{{ statusLabel(item.status) }}</td><td><button class="btn btn-sm" @click="selectedId = item.id">设置授权</button><template v-if="item.status === 'pending'"><button class="btn btn-sm btn-primary" @click="review(item.id, 'approve')">批准</button><button class="btn btn-sm btn-danger" @click="review(item.id, 'reject')">驳回</button></template></td></tr><tr v-if="!incoming.length"><td colspan="3" class="lan-table-empty">暂无连接申请</td></tr></tbody></table></div>
      <div v-if="selected && selected.status !== 'rejected'" class="lan-remote-grants"><h3 class="h3">{{ selected.source_name }} 的路径授权</h3><form class="lan-remote-grant-form" @submit.prevent="saveGrant"><div><label class="label" for="remote-path">文件夹路径</label><div class="field is-path"><input id="remote-path" v-model="grant.path" required placeholder="/共享资料" /></div></div><div class="lan-permissions"><span class="label">允许的操作</span><label v-for="key in (['upload','download','modify','delete'] as Permission[])" :key="key"><input v-model="grant[key]" type="checkbox" />{{ labels[key] }}</label></div><button class="btn btn-primary">保存授权</button></form><div v-for="item in selected.grants" :key="item.id" class="lan-remote-grant-row"><span class="mono">/{{ item.path }}</span><span class="sub">{{ (['upload','download','modify','delete'] as Permission[]).filter(key => key === 'delete' ? item.delete_allowed : item[key]).map(key => labels[key]).join(' · ') }}</span><button class="btn btn-sm btn-danger" @click="removeGrant(item.id)">撤销</button></div><p v-if="!selected.grants.length" class="sub">尚未开放路径。</p></div>
    </section>
  </div>
</template>
