<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { api, type RegistrationRequest, type TrustedDevice } from '../api'
import { authStatus, authTime } from '../lib/auth'

const registrations = ref<RegistrationRequest[]>([]), devices = ref<TrustedDevice[]>([])
const emit = defineEmits<{ changed: [] }>()
const error = ref(''), notice = ref(''), busy = ref(false), revoking = ref<TrustedDevice | null>(null)
async function refresh() {
  try { const [r, d] = await Promise.all([api.registrations(), api.deviceRequests()]); registrations.value = r.requests; devices.value = d.devices }
  catch (e) { error.value = e instanceof Error ? e.message : '审核列表读取失败' }
}
async function review(id: number, decision: 'approve' | 'reject') {
  if (busy.value) return
  busy.value = true; error.value = ''; notice.value = ''
  try {
    await api.reviewRegistration(id, decision)
    notice.value = decision === 'approve' ? '账号已通过审核，已有个人空间和公共区只读权限；公共区写权限需另行授权' : '已驳回申请'
    await refresh()
    emit('changed')
  } catch (e) { error.value = e instanceof Error ? e.message : '审核失败' }
  finally { busy.value = false }
}
async function revoke() {
  if (!revoking.value || busy.value) return
  busy.value = true; error.value = ''
  try { await api.revokeTrustedDevice(revoking.value.id); revoking.value = null; notice.value = '免密资格和关联会话已撤销'; await refresh() }
  catch (e) { error.value = e instanceof Error ? e.message : '撤销失败' }
  finally { busy.value = false }
}
onMounted(refresh)
defineExpose({ refresh })
</script>

<template>
  <div class="lan-review-card lan-account-approvals">
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }}<button aria-label="关闭审核错误" @click="error = ''">×</button></p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}<button aria-label="关闭审核提示" @click="notice = ''">×</button></p>
    <section class="card"><div class="card-head"><div><h2 class="h3">注册申请审核</h2><p class="sub">创建普通账号，默认拥有个人空间和公共区只读权限；公共区写权限需管理员授权，无需另审浏览器。</p></div><div class="spacer" /><button class="btn btn-sm" :disabled="busy" @click="refresh">刷新审核列表</button></div>
      <div class="lan-table-wrap"><table class="files"><thead><tr><th>申请账号</th><th>设备</th><th>申请时间</th><th>状态</th><th>操作</th></tr></thead><tbody>
        <tr v-for="item in registrations" :key="item.id"><td data-label="账号">{{ item.username }}</td><td data-label="设备">{{ item.device_name }} <span v-if="item.trust_device" class="chip chip-ok">自动记住浏览器</span></td><td data-label="提交时间">{{ authTime(item.created_at) }}</td><td data-label="状态">{{ authStatus[item.status] }}</td><td data-label="操作"><template v-if="item.status === 'pending'"><button class="btn btn-sm btn-primary" :disabled="busy" @click="review(item.id, 'approve')">通过注册</button> <button class="btn btn-sm btn-danger" :disabled="busy" @click="review(item.id, 'reject')">驳回注册</button></template></td></tr>
        <tr v-if="!registrations.length"><td colspan="5" class="lan-table-empty">暂无注册申请</td></tr>
      </tbody></table></div>
    </section>
    <section class="card lan-review-card"><div class="card-head"><div><h2 class="h3">浏览器登录管理</h2><p class="sub">密码登录后自动记住，无需单独审批。可在此撤销登录凭据；管理员浏览器仅由超级管理员管理。</p></div></div>
      <div class="lan-table-wrap"><table class="files"><thead><tr><th>账号</th><th>设备</th><th>状态</th><th>最近使用</th><th>操作</th></tr></thead><tbody>
        <tr v-for="item in devices" :key="item.id"><td data-label="账号">{{ item.username }}</td><td data-label="设备">{{ item.name }}</td><td data-label="状态">{{ item.status === 'pending' ? '旧版待审，现无需审批' : authStatus[item.status] }}</td><td data-label="最近使用">{{ authTime(item.last_used_at) }}</td><td data-label="操作"><button v-if="['pending', 'approved'].includes(item.status)" class="btn btn-sm btn-danger" :disabled="busy" @click="revoking = item">撤销免密</button></td></tr>
        <tr v-if="!devices.length"><td colspan="5" class="lan-table-empty">暂无浏览器登录记录</td></tr>
      </tbody></table></div>
    </section>
  </div>
  <div v-if="revoking" class="scrim" @click.self="revoking = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="管理员撤销免密"><div class="sheet-head"><h2 class="h3">撤销 {{ revoking.username }} 的 {{ revoking.name }}？</h2></div><div class="sheet-body"><p class="sub">该浏览器的免密资格及关联会话将立即失效。</p></div><div class="sheet-foot"><button class="btn" @click="revoking = null">取消</button><div class="spacer" /><button class="btn btn-danger" :disabled="busy" @click="revoke">确认撤销</button></div></section></div>
</template>
