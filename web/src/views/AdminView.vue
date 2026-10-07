<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { api, type Grant, type PasswordRequest, type Permission, type User } from '../api'
import Icon from '../components/Icon.vue'
import SelectMenu from '../components/SelectMenu.vue'

const props = defineProps<{ currentUser: User }>()
type GrantRow = Grant & { username: string }
const users = ref<User[]>([])
const grants = ref<GrantRow[]>([])
const passwordRequests = ref<PasswordRequest[]>([])
const loading = ref(false)
const error = ref('')
const notice = ref('')
const newUser = reactive({ username: '', password: '', role: 'user' as User['role'] })
const grant = reactive({ user_id: 0, path: '/', upload: false, download: false, modify: false, delete: false })
const removingUser = ref<User | null>(null)
const removingGrant = ref<GrantRow | null>(null)
const regularUsers = computed(() => users.value.filter(user => user.role === 'user'))
const roleOptions = computed(() => props.currentUser.role === 'superadmin'
  ? [{ value: 'user', label: '普通用户' }, { value: 'admin', label: '管理员' }]
  : [{ value: 'user', label: '普通用户' }])
const regularUserOptions = computed(() => regularUsers.value.map(user => ({ value: user.id, label: user.username })))
const permissionLabels: Record<Permission, string> = { upload: '上传', download: '下载', modify: '修改', delete: '删除' }
const errorText = (e: unknown) => e instanceof Error ? e.message : '操作失败'
const roleLabel = (role: User['role']) => ({ superadmin: '超级管理员', admin: '管理员', user: '普通用户' })[role]

async function refresh() {
  loading.value = true
  error.value = ''
  try {
    users.value = (await api.users()).users
    if (!regularUsers.value.some(user => user.id === grant.user_id)) grant.user_id = regularUsers.value[0]?.id || 0
    const all = await Promise.all(regularUsers.value.map(async user =>
      (await api.grants(user.id)).grants.map(item => ({ ...item, username: user.username })),
    ))
    grants.value = all.flat()
    passwordRequests.value = (await api.passwordRequests()).requests
  } catch (e) { error.value = errorText(e) }
  finally { loading.value = false }
}
onMounted(() => void refresh())

async function createUser() {
  error.value = ''
  notice.value = ''
  try {
    await api.addUser(newUser.username.trim(), newUser.password, newUser.role)
    newUser.username = ''; newUser.password = ''; newUser.role = 'user'
    notice.value = '用户已创建'
    await refresh()
  } catch (e) { error.value = errorText(e) }
}
async function deleteUser() {
  if (!removingUser.value) return
  try { await api.removeUser(removingUser.value.id); removingUser.value = null; notice.value = '用户已删除'; await refresh() }
  catch (e) { error.value = errorText(e) }
}
async function saveGrant() {
  error.value = ''
  notice.value = ''
  if (!grant.user_id) { error.value = '请先创建普通用户'; return }
  try {
    await api.setGrant({ ...grant })
    notice.value = '授权已保存'
    await refresh()
  } catch (e) { error.value = errorText(e) }
}
async function revokeGrant() {
  if (!removingGrant.value) return
  try { await api.removeGrant(removingGrant.value.id); removingGrant.value = null; notice.value = '授权已撤销'; await refresh() }
  catch (e) { error.value = errorText(e) }
}
async function reviewPassword(id: number, decision: 'approve' | 'reject') {
  try {
    await api.reviewPassword(id, decision)
    notice.value = decision === 'approve' ? '已重置为临时密码 123456，有效期 30 分钟' : '已驳回申请'
    await refresh()
  } catch (e) { error.value = errorText(e) }
}
function enabledPermissions(item: GrantRow): Permission[] {
  return (['upload', 'download', 'modify', 'delete'] as Permission[]).filter(key => key === 'delete' ? !!item.delete_allowed : !!item[key])
}
</script>

<template>
  <header class="topbar"><span class="eyebrow">Access control / LAN</span><div class="spacer" /><button class="btn" :disabled="loading" @click="refresh"><Icon name="refresh" />刷新</button></header>
  <div class="page lan-admin">
    <div class="page-head"><div><h1 class="h1">用户与权限</h1><p class="sub">为普通用户分配指定路径的访问能力。</p></div></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }} <button @click="error = ''">×</button></p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }} <button @click="notice = ''">×</button></p>
    <div class="lan-admin-grid">
      <section class="card"><div class="card-head"><Icon name="user" /><h2 class="h3">创建用户</h2></div><form class="card-pad lan-form" @submit.prevent="createUser"><label class="label" for="new-username">用户名</label><div class="field"><input id="new-username" v-model="newUser.username" required placeholder="输入用户名" /></div><label class="label" for="new-password">初始密码（至少 6 位）</label><div class="field"><input id="new-password" v-model="newUser.password" required minlength="6" type="password" placeholder="设置初始密码" /></div><label class="label" for="new-role">角色</label><SelectMenu id="new-role" v-model="newUser.role" :options="roleOptions" /><button class="btn btn-primary" type="submit"><Icon name="plus" />创建用户</button></form></section>
      <section class="card"><div class="card-head"><Icon name="devices" /><h2 class="h3">现有用户</h2><div class="spacer" /><span class="mono sub">{{ users.length }} 个账号</span></div><div class="lan-table-wrap"><table class="files"><thead><tr><th>用户</th><th>角色</th><th>操作</th></tr></thead><tbody><tr v-for="user in users" :key="user.id"><td><div class="file-cell"><span class="me-av lan-user-avatar">{{ user.username.slice(0, 2).toUpperCase() }}</span><strong>{{ user.username }}</strong></div></td><td><span class="chip" :class="user.role !== 'user' ? 'chip-ok' : 'chip-idle'">{{ roleLabel(user.role) }}</span></td><td class="col-act"><button v-if="user.id !== props.currentUser.id && user.role !== 'superadmin' && (user.role === 'user' || props.currentUser.role === 'superadmin')" class="btn btn-sm btn-danger" @click="removingUser = user">删除</button></td></tr></tbody></table></div></section>
    </div>
    <section class="card lan-grants-card"><div class="card-head"><Icon name="shield" /><div><h2 class="h3">路径授权</h2><p class="sub">授权作用于所选文件夹及其子路径。多条授权的权限会合并。</p></div></div><form class="card-pad lan-grant-form" @submit.prevent="saveGrant"><div><label class="label" for="grant-user">普通用户</label><SelectMenu id="grant-user" v-model="grant.user_id" :options="regularUserOptions" :disabled="!regularUsers.length" /></div><div><label class="label" for="grant-path">文件夹路径</label><div class="field is-path"><input id="grant-path" v-model="grant.path" required placeholder="/共享资料" /></div></div><div class="lan-permissions"><span class="label">允许的操作</span><label v-for="key in (['upload', 'download', 'modify', 'delete'] as Permission[])" :key="key"><input v-model="grant[key]" type="checkbox" />{{ permissionLabels[key] }}</label></div><button class="btn btn-primary" :disabled="!regularUsers.length" type="submit">保存授权</button></form><div class="lan-grant-note"><Icon name="shield" />上传允许新建；修改允许覆盖、移动和重命名；删除允许移除目录及其内容。</div><div class="lan-table-wrap"><table class="files"><thead><tr><th>用户</th><th>授权路径</th><th>权限</th><th>操作</th></tr></thead><tbody><tr v-for="item in grants" :key="item.id"><td>{{ item.username }}</td><td class="mono">/{{ item.path }}</td><td><span v-for="key in enabledPermissions(item)" :key="key" class="chip chip-ok lan-permission-chip">{{ permissionLabels[key] }}</span><span v-if="!enabledPermissions(item).length" class="sub">无</span></td><td class="col-act"><button class="btn btn-sm btn-danger" @click="removingGrant = item">撤销</button></td></tr><tr v-if="!grants.length"><td colspan="4" class="lan-table-empty">暂无路径授权</td></tr></tbody></table></div></section>
    <section class="card lan-review-card">
      <div class="card-head"><Icon name="lock" /><div><h2 class="h3">忘记密码审核</h2><p class="sub">普通用户由管理员或超级管理员审核；管理员由超级管理员审核。</p></div></div>
      <div class="lan-table-wrap"><table class="files"><thead><tr><th>申请账号</th><th>角色</th><th>提交时间</th><th>状态</th><th>操作</th></tr></thead><tbody>
        <tr v-for="item in passwordRequests" :key="item.id"><td>{{ item.username }}</td><td>{{ roleLabel(item.role) }}</td><td>{{ new Date(item.created_at * 1000).toLocaleString('zh-CN') }}</td><td>{{ item.status === 'pending' ? '待审核' : item.status === 'approved' ? '已通过' : '已驳回' }}</td><td class="col-act"><template v-if="item.status === 'pending'"><button class="btn btn-sm btn-primary" @click="reviewPassword(item.id, 'approve')">通过</button> <button class="btn btn-sm btn-danger" @click="reviewPassword(item.id, 'reject')">驳回</button></template></td></tr>
        <tr v-if="!passwordRequests.length"><td colspan="5" class="lan-table-empty">暂无申请</td></tr>
      </tbody></table></div>
    </section>
  </div>
  <div v-if="removingUser" class="scrim" @click.self="removingUser = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="删除用户"><div class="sheet-head"><h2 class="h3">删除用户 {{ removingUser.username }}？</h2></div><div class="sheet-body"><p class="sub">该账号将无法继续登录，已有授权会一并撤销。</p></div><div class="sheet-foot"><div class="spacer" /><button class="btn" @click="removingUser = null">取消</button><button class="btn btn-danger" @click="deleteUser">删除用户</button></div></section></div>
  <div v-if="removingGrant" class="scrim" @click.self="removingGrant = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="撤销授权"><div class="sheet-head"><h2 class="h3">撤销路径授权？</h2></div><div class="sheet-body"><p class="sub">{{ removingGrant.username }} 对 /{{ removingGrant.path }} 的这条授权将被撤销。</p></div><div class="sheet-foot"><div class="spacer" /><button class="btn" @click="removingGrant = null">取消</button><button class="btn btn-danger" @click="revokeGrant">撤销授权</button></div></section></div>
</template>
