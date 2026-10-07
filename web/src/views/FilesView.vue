<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { api, ApiError, downloadUrl, uploadFile, type FileEntry, type FileList, type Permission } from '../api'
import Icon from '../components/Icon.vue'

const props = defineProps<{ remoteId?: number | null; serverName: string }>()

const listing = ref<FileList>({ path: '', permissions: [], items: [] })
const loaded = ref(false)
const loading = ref(false)
const search = ref('')
const error = ref('')
const notice = ref('')
const creating = ref(false)
const newName = ref('')
const moving = ref<FileEntry | null>(null)
const destination = ref('')
const deleting = ref<FileEntry | null>(null)
const uploading = ref(false)
const uploadLabel = ref('')
const uploadPercent = ref(0)
const dragHot = ref(false)
const fileInput = ref<HTMLInputElement | null>(null)
const folderInput = ref<HTMLInputElement | null>(null)

const filtered = computed(() => {
  const needle = search.value.trim().toLocaleLowerCase()
  return needle ? listing.value.items.filter(item => item.name.toLocaleLowerCase().includes(needle)) : listing.value.items
})
const folders = computed(() => listing.value.items.filter(item => item.directory).length)
const files = computed(() => listing.value.items.length - folders.value)
const crumbs = computed(() => {
  const result = [{ name: '全部文件', path: '' }]
  let path = ''
  for (const segment of listing.value.path.split('/').filter(Boolean)) {
    path = [path, segment].filter(Boolean).join('/')
    result.push({ name: segment, path })
  }
  return result
})
const has = (permission: Permission) => listing.value.permissions.includes(permission)
const join = (a: string, b: string) => [a, b].filter(Boolean).join('/')
const errorText = (e: unknown) => e instanceof Error ? e.message : '操作失败'
const formatBytes = (bytes: number | null) => bytes === null ? '—' : bytes < 1024 ? `${bytes} B` : bytes < 1024 ** 2 ? `${(bytes / 1024).toFixed(1)} KB` : bytes < 1024 ** 3 ? `${(bytes / 1024 ** 2).toFixed(1)} MB` : `${(bytes / 1024 ** 3).toFixed(2)} GB`
const formatWhen = (seconds: number) => new Date(seconds * 1000).toLocaleString('zh-CN')
const fileIcon = (item: FileEntry) => item.directory ? 'folder' : /\.(png|jpe?g|gif|webp|svg)$/i.test(item.name) ? 'image' : /\.(zip|7z|tar|gz|rar)$/i.test(item.name) ? 'archive' : 'file'

async function open(path = listing.value.path) {
  loading.value = true
  error.value = ''
  try { listing.value = await api.list(path, props.remoteId); loaded.value = true }
  catch (e) { error.value = errorText(e) }
  finally { loading.value = false }
}
onMounted(() => void open(''))

function download(item: FileEntry) {
  if (!item.permissions.includes('download')) { error.value = '该文件没有下载权限'; return }
  window.location.href = downloadUrl(item.path, item.directory, props.remoteId)
}
function downloadCurrent() { window.location.href = downloadUrl(listing.value.path, true, props.remoteId) }
function openEntry(item: FileEntry) {
  if (item.directory) void open(item.path)
  else download(item)
}
async function createFolder() {
  const name = newName.value.trim()
  if (!name) return
  try { await api.mkdir(join(listing.value.path, name), props.remoteId); creating.value = false; newName.value = ''; notice.value = '文件夹已创建'; await open() }
  catch (e) { error.value = errorText(e) }
}
function startMove(item: FileEntry) { moving.value = item; destination.value = item.path }
async function submitMove() {
  if (!moving.value) return
  try { await api.move(moving.value.path, destination.value.trim(), props.remoteId); moving.value = null; notice.value = '移动完成'; await open() }
  catch (e) { error.value = errorText(e) }
}
async function submitDelete() {
  if (!deleting.value) return
  try { await api.remove(deleting.value.path, props.remoteId); deleting.value = null; notice.value = '删除完成'; await open() }
  catch (e) { error.value = errorText(e) }
}

async function uploadFiles(selected: File[], folder: boolean) {
  if (!selected.length || uploading.value) return
  const created = new Set<string>()
  const targetFolder = listing.value.path
  error.value = ''
  notice.value = ''
  uploading.value = true
  try {
    for (let index = 0; index < selected.length; index++) {
      const file = selected[index]
      const relative = folder ? file.webkitRelativePath : file.name
      const segments = relative.split('/')
      let parent = targetFolder
      for (const segment of segments.slice(0, -1)) {
        parent = join(parent, segment)
        if (created.has(parent)) continue
        try { await api.mkdir(parent, props.remoteId) }
        catch (e) { if (!(e instanceof ApiError && e.status === 409)) throw e }
        created.add(parent)
      }
      uploadLabel.value = `${index + 1} / ${selected.length} · ${relative}`
      uploadPercent.value = 0
      await uploadFile(file, join(targetFolder, relative), (sent, total) => { uploadPercent.value = total ? Math.round(sent / total * 100) : 0 }, props.remoteId)
    }
    notice.value = `已上传 ${selected.length} 个文件`
    await open()
  } catch (e) { const message = errorText(e); await open(); error.value = message }
  finally { uploading.value = false; if (fileInput.value) fileInput.value.value = ''; if (folderInput.value) folderInput.value.value = '' }
}
function chooseFiles(event: Event, folder: boolean) {
  const input = event.target as HTMLInputElement
  void uploadFiles(Array.from(input.files || []), folder)
}
function dragOver(event: DragEvent) { event.preventDefault(); dragHot.value = true }
function dragLeave(event: DragEvent) {
  if (!(event.currentTarget as HTMLElement).contains(event.relatedTarget as Node | null)) dragHot.value = false
}
function drop(event: DragEvent) {
  event.preventDefault()
  dragHot.value = false
  if (!has('upload') && !has('modify')) { error.value = '当前路径没有上传权限'; return }
  const entries = Array.from(event.dataTransfer?.items || []).map(item => item.webkitGetAsEntry?.())
  if (entries.some(entry => entry?.isDirectory)) { error.value = '文件夹请使用“上传文件夹”按钮'; return }
  void uploadFiles(Array.from(event.dataTransfer?.files || []), false)
}
</script>

<template>
  <header class="topbar lan-topbar">
    <div class="field search-field"><Icon name="search" /><input v-model="search" placeholder="搜索当前文件夹" aria-label="搜索当前文件夹" /></div>
    <div class="spacer" />
    <button class="btn" :disabled="loading" @click="open()"><Icon name="refresh" />刷新</button>
    <button class="btn" :disabled="!has('upload')" @click="creating = true"><Icon name="plus" />新建文件夹</button>
    <button class="btn btn-primary" :disabled="!has('upload') && !has('modify')" @click="fileInput?.click()"><Icon name="upload" />上传文件</button>
  </header>
  <div class="page">
    <div class="page-head"><div><h1 class="h1">{{ serverName }} · 文件区</h1><p class="sub">管理内网文件与文件夹。</p></div><div class="spacer" /><span class="chip chip-ok">{{ remoteId == null ? '本地服务器' : '外连服务器' }}</span></div>
    <p v-if="error" class="lan-message is-error" role="alert">{{ error }} <button @click="error = ''">×</button></p>
    <p v-if="notice" class="lan-message is-ok" role="status">{{ notice }} <button @click="notice = ''">×</button></p>
    <div class="card">
      <div class="card-head lan-file-head">
        <div class="crumbs"><template v-for="(crumb, index) in crumbs" :key="crumb.path"><a v-if="index < crumbs.length - 1" href="#" @click.prevent="open(crumb.path)">{{ crumb.name }}</a><b v-else>{{ crumb.name }}</b><span v-if="index < crumbs.length - 1" class="sep">/</span></template></div>
        <div class="spacer" /><span class="mono sub">{{ folders }} 个文件夹 · {{ files }} 个文件</span>
      </div>
      <div class="lan-actions">
        <button class="btn btn-sm" :disabled="!has('upload')" @click="folderInput?.click()"><Icon name="upload" />上传文件夹</button>
        <button class="btn btn-sm" :disabled="!has('download')" @click="downloadCurrent"><Icon name="download" />下载当前文件夹</button>
        <span class="spacer" /><span class="sub">当前路径权限：{{ listing.permissions.length ? listing.permissions.map(p => ({ upload: '上传', download: '下载', modify: '修改', delete: '删除' })[p]).join(' · ') : '仅浏览' }}</span>
      </div>
      <input ref="fileInput" type="file" multiple hidden @change="chooseFiles($event, false)" />
      <input ref="folderInput" type="file" webkitdirectory directory multiple hidden @change="chooseFiles($event, true)" />
      <div v-if="uploading" class="lan-progress"><div class="mono">正在上传 {{ uploadLabel }} · {{ uploadPercent }}%</div><div class="prog"><span :style="{ width: `${uploadPercent}%` }" /></div></div>
      <div v-if="creating" class="card-pad lan-create"><div class="field"><Icon name="folder" /><input v-model="newName" autofocus placeholder="文件夹名称" @keyup.enter="createFolder" @keyup.esc="creating = false" /></div><button class="btn btn-primary btn-sm" @click="createFolder">创建</button><button class="btn btn-sm" @click="creating = false">取消</button></div>
      <div class="lan-list" :class="{ 'is-hot': dragHot }" @dragover="dragOver" @dragleave="dragLeave" @drop="drop">
        <div v-if="!loaded" class="empty"><div class="empty-mark"><Icon name="refresh" /></div><div class="empty-title">正在读取…</div></div>
        <div v-else-if="!filtered.length" class="empty"><div class="empty-mark"><Icon name="folder" /></div><div class="empty-title">{{ search ? '没有匹配的文件' : '这个文件夹是空的' }}</div><p class="empty-sub sub">{{ search ? '换个关键词试试。' : '上传文件或新建文件夹，开始整理内容。' }}</p></div>
        <div v-else class="lan-table-wrap"><table class="files"><thead><tr><th>名称</th><th>大小</th><th>修改时间</th><th>操作</th></tr></thead><tbody><tr v-for="item in filtered" :key="item.path"><td><button class="file-cell lan-file-button" @click="openEntry(item)"><span class="file-ico" :class="{ 'is-dir': item.directory }"><Icon :name="fileIcon(item)" /></span><span class="file-name">{{ item.name }}</span></button></td><td class="col-size">{{ item.directory ? '—' : formatBytes(item.size) }}</td><td class="col-time">{{ formatWhen(item.modified) }}</td><td class="col-act"><button v-if="item.permissions.includes('download')" class="btn btn-sm btn-quiet" :aria-label="`下载 ${item.name}`" @click="download(item)"><Icon name="download" /></button><button v-if="item.permissions.includes('modify')" class="btn btn-sm btn-quiet" :aria-label="`移动或重命名 ${item.name}`" @click="startMove(item)"><Icon name="transfer" /></button><button v-if="item.permissions.includes('delete')" class="btn btn-sm btn-quiet btn-danger" :aria-label="`删除 ${item.name}`" @click="deleting = item"><Icon name="minus" /></button></td></tr></tbody></table></div>
        <div v-if="dragHot" class="lan-drop-hint">松开以上传文件</div>
      </div>
    </div>
    <p class="sub lan-footnote">文件传输在内网直连完成；文件夹下载为 ZIP。</p>
  </div>

  <div v-if="moving" class="scrim" @click.self="moving = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="移动或重命名"><div class="sheet-head"><h2 class="h3">移动或重命名</h2><p class="sub">输入相对根目录的目标路径</p></div><div class="sheet-body"><label class="label" for="destination">目标路径</label><div class="field is-path"><input id="destination" v-model="destination" autofocus @keyup.enter="submitMove" /></div></div><div class="sheet-foot"><div class="spacer" /><button class="btn" @click="moving = null">取消</button><button class="btn btn-primary" @click="submitMove">保存</button></div></section></div>
  <div v-if="deleting" class="scrim" @click.self="deleting = null"><section class="sheet" role="dialog" aria-modal="true" aria-label="确认删除"><div class="sheet-head"><h2 class="h3">删除 {{ deleting.name }}？</h2></div><div class="sheet-body"><p class="sub">{{ deleting.directory ? '此文件夹及其全部内容将被永久删除。' : '此文件将被永久删除。' }}</p></div><div class="sheet-foot"><div class="spacer" /><button class="btn" @click="deleting = null">取消</button><button class="btn btn-danger" @click="submitDelete">确认删除</button></div></section></div>
</template>
