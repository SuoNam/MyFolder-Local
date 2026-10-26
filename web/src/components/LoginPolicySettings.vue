<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { api, type User } from '../api'
import { loginDuration } from '../lib/auth'
import SelectMenu from './SelectMenu.vue'

const props = defineProps<{ role: User['role'] }>()
const current = ref<number | null>(null), days = ref(90)
const mode = ref<string | number>('days')
const busy = ref(false), confirming = ref(false), error = ref(''), notice = ref('')
const selected = computed(() => mode.value === 'unlimited' ? 0 : days.value)
const dirty = computed(() => current.value !== null && selected.value !== current.value)
const options = [{ value: 'days', label: '指定有效天数' }, { value: 'unlimited', label: '长期有效（直到撤销）' }]
function assign(value: number) {
  current.value = value; mode.value = value === 0 ? 'unlimited' : 'days'; days.value = value || 90
}
async function refresh() {
  error.value = ''
  try { assign((await api.loginPolicy()).device_days) }
  catch (e) { error.value = e instanceof Error ? e.message : '读取免密策略失败' }
}
function confirm() {
  error.value = ''; notice.value = ''
  if (!Number.isInteger(selected.value) || selected.value < 0 || selected.value > 3650 || (mode.value === 'days' && selected.value === 0)) {
    error.value = '请填写 1–3650 天的整数，或选择长期有效'; return
  }
  confirming.value = true
}
async function save() {
  if (busy.value || !dirty.value) return
  busy.value = true; error.value = ''
  try {
    const result = await api.saveLoginPolicy(selected.value)
    assign(result.device_days); confirming.value = false
    notice.value = `免密策略已保存：${loginDuration(result.device_days)}，已更新 ${result.updated_browsers || 0} 个有效浏览器`
  } catch (e) { error.value = e instanceof Error ? e.message : '保存免密策略失败' }
  finally { busy.value = false }
}
onMounted(refresh)
</script>

<template>
  <section class="card lan-review-card" aria-label="浏览器免密设置">
    <div class="card-head"><div><h2 class="h3">浏览器免密设置</h2><p class="sub">全服务器规则，仅超级管理员可以修改；普通管理员可查看。</p></div></div>
    <div class="card-pad">
      <p v-if="error" class="lan-message is-error" role="alert">{{ error }}</p>
      <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}</p>
      <p v-if="current !== null" class="sub">当前免密有效期：{{ loginDuration(current) }}</p>
      <form v-if="props.role === 'superadmin' && current !== null" class="lan-form" @submit.prevent="confirm">
        <label class="label" for="login-lifetime-mode">免密期限模式</label>
        <SelectMenu id="login-lifetime-mode" v-model="mode" :options="options" :disabled="busy" />
        <template v-if="mode === 'days'"><label class="label" for="login-lifetime-days">有效天数</label><div class="field"><input id="login-lifetime-days" v-model.number="days" type="number" min="1" max="3650" step="1" required :disabled="busy" /></div></template>
        <p class="sub">更改后，当前有效浏览器从保存时按新规则重新计时；其他浏览器下次访问时更新本地 Cookie。过期、撤销及驳回记录不会恢复，保存相同设置不会延长期限。</p>
        <p class="sub">长期有效仅指服务器不主动到期，访问时刷新 Cookie；清除浏览器数据、主动退出、管理员撤销或重置密码仍会失效。浏览器本身的保存期限也可能更短。</p>
        <button class="btn btn-primary" :disabled="busy || !dirty" type="submit">保存免密设置</button>
      </form>
    </div>
  </section>
  <div v-if="confirming" class="scrim" @click.self="!busy && (confirming = false)"><section class="sheet" role="dialog" aria-modal="true" aria-label="确认修改免密有效期"><div class="sheet-head"><h2 class="h3">修改为{{ loginDuration(selected) }}？</h2></div><div class="sheet-body"><p class="sub">这会修改全服务器规则，并对当前有效浏览器重新计时。不会恢复已过期或已撤销的凭据。</p></div><div class="sheet-foot"><button class="btn" :disabled="busy" @click="confirming = false">取消</button><div class="spacer" /><button class="btn btn-primary" :disabled="busy" @click="save">确认保存</button></div></section></div>
</template>
