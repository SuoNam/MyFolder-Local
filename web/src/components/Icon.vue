<template>
  <svg class="ico" aria-hidden="true"><use :href="`#i-${name}`" /></svg>
</template>
<script setup lang="ts">
defineProps<{ name: string }>()

// Mount the sprite sheet once; every <Icon> then references it by id.
import spriteUrl from './icons.svg?raw'
if (!document.getElementById('mf-sprite')) {
  const host = document.createElement('div')
  host.id = 'mf-sprite'
  host.innerHTML = spriteUrl
  document.body.prepend(host)
}
</script>
