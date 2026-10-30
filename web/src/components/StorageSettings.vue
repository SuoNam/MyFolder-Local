<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { api, type StorageUser } from '../api'
import SelectMenu from './SelectMenu.vue'

const users = ref<StorageUser[]>([]), selected = ref<string | number>(0)
const target = computed(() => users.value.find(item => item.id === selected.value))
const options = computed(() => users.value.map(item => ({ value: item.id, label: item.username })))
const quotaGb = ref(5), upload = ref(false), modify = ref(false), remove = ref(false)
const busy = ref(false), error = ref(''), notice = ref('')
const dirty = computed(() => !!target.value && (Math.round(quotaGb.value * 1024 ** 3) !== target.value.storage_limit ||
  upload.value !== target.value.public_upload || modify.value !== target.value.public_modify || remove.value !== target.value.public_delete))
let refreshNumber = 0
function fill() {
  if (!target.value) return
  quotaGb.value = target.value.storage_limit / 1024 ** 3
  upload.value = target.value.public_upload; modify.value = target.value.public_modify; remove.value = target.value.public_delete
}
watch(selected, fill)
async function refresh() {
  error.value = ''
  const number = ++refreshNumber
  try {
    const result = await api.storageUsers()
    if (number !== refreshNumber) return
    const keepDraft = dirty.value
    users.value = result.users
    if (!users.value.some(item => item.id === selected.value)) { selected.value = users.value[0]?.id || 0; fill() }
    else if (!keepDraft) fill()
  } catch (e) { if (number === refreshNumber) error.value = e instanceof Error ? e.message : '读取空间配置失败' }
}
async function save() {
  if (busy.value || !target.value) return
  const bytes = Math.round(quotaGb.value * 1024 ** 3)
  if (!Number.isFinite(bytes) || bytes < 1 || bytes > 1024 ** 5) { error.value = '配额须在 1 字节至 1 PB 之间'; return }
  busy.value = true; error.value = ''; notice.value = ''
  try {
    await api.saveStorageUser({ user_id: target.value.id, storage_limit: bytes, public_upload: upload.value, public_modify: modify.value, public_delete: remove.value })
    await refresh(); notice.value = '个人配额和公共区权限已保存'
  } catch (e) { error.value = e instanceof Error ? e.message : '保存空间配置失败' }
  finally { busy.value = false }
}
onMounted(refresh)
defineExpose({ refresh })
</script>

<template>
  <section class="card lan-review-card" aria-label="个人配额与公共区权限">
    <div class="card-head"><div><h2 class="h3">个人配额与公共区权限</h2><p class="sub">每个账号默认 5 GB 独立空间，公共区对所有登录用户可见。</p></div></div>
    <div class="card-pad">
      <p v-if="error" class="lan-message is-error" role="alert">{{ error }}</p>
      <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}</p>
      <form class="lan-form" @submit.prevent="save">
        <label class="label" for="storage-user">用户</label><SelectMenu id="storage-user" v-model="selected" :options="options" :disabled="busy || !users.length" />
        <template v-if="target">
          <p class="sub">个人空间已使用 {{ (target.storage_used / 1024 ** 3).toFixed(2) }} GB / {{ (target.storage_limit / 1024 ** 3).toFixed(2) }} GB；此处仅管理配额，不开放他人私人文件。</p>
          <label class="label" for="personal-quota">个人空间配额（GB）</label><div class="field"><input id="personal-quota" v-model.number="quotaGb" type="number" min="0.000000001" max="1048576" step="any" required :disabled="busy" /></div>
          <div class="lan-permissions"><span class="label">公共区写权限</span><label><input v-model="upload" type="checkbox" :disabled="busy || target.role !== 'user'" />上传 / 新建</label><label><input v-model="modify" type="checkbox" :disabled="busy || target.role !== 'user'" />修改 / 移动</label><label><input v-model="remove" type="checkbox" :disabled="busy || target.role !== 'user'" />删除</label></div>
          <p class="sub">公共区浏览、下载无需额外授权；管理员已有公共区完整管理权限。缩小配额不会删除文件，超额后仍可下载、删除或缩小文件，但不能继续增长。服务器总容量限制仍然生效。</p>
          <button class="btn btn-primary" :disabled="busy" type="submit">保存空间配置</button>
        </template>
      </form>
    </div>
  </section>
</template>
