<script setup lang="ts">
import { nextTick, onMounted, onUnmounted, ref } from 'vue'

type Option = { value: string | number; label: string }
const props = defineProps<{
  id: string
  modelValue: string | number
  options: Option[]
  disabled?: boolean
}>()
const emit = defineEmits<{ 'update:modelValue': [value: string | number] }>()
const root = ref<HTMLElement | null>(null)
const trigger = ref<HTMLButtonElement | null>(null)
const optionButtons = ref<HTMLButtonElement[]>([])
const open = ref(false)

function close() { open.value = false }
function onOutside(event: PointerEvent) {
  if (root.value && !root.value.contains(event.target as Node)) close()
}
onMounted(() => document.addEventListener('pointerdown', onOutside))
onUnmounted(() => document.removeEventListener('pointerdown', onOutside))

async function show() {
  if (props.disabled || !props.options.length) return
  open.value = true
  await nextTick()
  const selected = props.options.findIndex(option => option.value === props.modelValue)
  optionButtons.value[Math.max(selected, 0)]?.focus()
}
function choose(option: Option) {
  emit('update:modelValue', option.value)
  close()
  nextTick(() => trigger.value?.focus())
}
function onKeydown(event: KeyboardEvent) {
  if (event.key === 'Escape' && open.value) {
    event.preventDefault()
    close()
    trigger.value?.focus()
  } else if ((event.key === 'ArrowDown' || event.key === 'ArrowUp') && open.value) {
    event.preventDefault()
    const current = optionButtons.value.indexOf(document.activeElement as HTMLButtonElement)
    const direction = event.key === 'ArrowDown' ? 1 : -1
    const next = (current + direction + props.options.length) % props.options.length
    optionButtons.value[next]?.focus()
  } else if ((event.key === 'ArrowDown' || event.key === 'ArrowUp') && !open.value) {
    event.preventDefault()
    void show()
  }
}
</script>

<template>
  <div ref="root" class="select-menu" @keydown="onKeydown">
    <button
      :id="id" ref="trigger" type="button" class="select-menu-trigger"
      :disabled="disabled" aria-haspopup="listbox" :aria-expanded="open"
      :aria-controls="`${id}-options`" @click="open ? close() : show()"
    >
      <span>{{ options.find(option => option.value === modelValue)?.label || '请选择' }}</span>
      <svg viewBox="0 0 12 12" aria-hidden="true"><path d="m3 4.5 3 3 3-3" /></svg>
    </button>
    <div v-if="open" :id="`${id}-options`" class="select-menu-options" role="listbox" :aria-labelledby="id">
      <button
        v-for="option in options" :key="option.value" ref="optionButtons"
        type="button" role="option" class="select-menu-option"
        :aria-selected="option.value === modelValue"
        @click="choose(option)"
      >
        <span>{{ option.label }}</span><span v-if="option.value === modelValue" class="select-menu-check">✓</span>
      </button>
    </div>
  </div>
</template>
