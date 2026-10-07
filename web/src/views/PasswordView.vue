<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue'
import { api, type User } from '../api'
import Icon from '../components/Icon.vue'

const props = defineProps<{ user: User }>()
const emit = defineEmits<{ changed: [user: User] }>()
const password = ref('')
const confirm = ref('')
const now = ref(Date.now())
const error = ref('')
const notice = ref('')
const remaining = computed(() => Math.max(0, Math.ceil((props.user.reset_until * 1000 - now.value) / 60000)))
let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { now.value = Date.now() }, 10000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

async function save() {
  error.value = ''
  notice.value = ''
  if (password.value !== confirm.value) { error.value = '两次输入的密码不一致'; return }
  try {
    await api.changePassword(password.value)
    emit('changed', await api.me())
    password.value = ''
    confirm.value = ''
    notice.value = '密码已修改，临时密码已失效'
  } catch (e) { error.value = e instanceof Error ? e.message : '修改失败' }
}
</script>

<template>
  <header class="topbar"><span class="eyebrow">Account / Password</span></header>
  <div class="page lan-password-page">
    <div class="page-head"><div><h1 class="h1">修改密码</h1><p class="sub">忘记密码申请获批后，可在 30 分钟内设置新密码。</p></div></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }}</p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }}</p>
    <section class="card"><div class="card-head"><Icon name="lock" /><h2 class="h3">设置新密码</h2></div>
      <form v-if="remaining" class="card-pad lan-form" @submit.prevent="save"><p class="sub">剩余约 {{ remaining }} 分钟。新密码至少 6 位，不能与临时密码相同。</p><label class="label" for="new-password">新密码</label><div class="field"><input id="new-password" v-model="password" type="password" minlength="6" required autocomplete="new-password" /></div><label class="label" for="confirm-password">确认新密码</label><div class="field"><input id="confirm-password" v-model="confirm" type="password" minlength="6" required autocomplete="new-password" /></div><button class="btn btn-primary">保存新密码</button></form>
      <div v-else class="card-pad"><p class="sub">修改窗口已结束。如仍使用临时密码，请重新提交忘记密码申请。</p></div>
    </section>
  </div>
</template>
