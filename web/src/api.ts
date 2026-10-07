export type Permission = 'upload' | 'download' | 'modify' | 'delete'
export type User = { id: number; username: string; role: 'superadmin' | 'admin' | 'user'; reset_until: number }
export type FileEntry = { name: string; path: string; directory: boolean; size: number | null; modified: number; permissions: Permission[] }
export type FileList = { path: string; permissions: Permission[]; items: FileEntry[] }
export type Grant = { id: number; user_id: number; path: string; upload: number; download: number; modify: number; delete_allowed: number }
export type ServerInfo = { name: string; server_id: string; storage_path: string; storage_limit: number; storage_used: number; file_count: number; remotes: { id: number; remote_name: string; status: string }[] }
export type PasswordRequest = { id: number; username: string; role: User['role']; status: string; created_at: number; reviewed_at: number | null }
export type RemoteGrant = { id: number; path: string; upload: number; download: number; modify: number; delete_allowed: number }
export type RemoteIncoming = { id: string; source_name: string; status: string; created_at: number; grants: RemoteGrant[] }
export type RemoteOutgoing = { id: number; url: string; remote_name: string; status: string; created_at: number }

export class ApiError extends Error {
  constructor(message: string, public status: number) { super(message) }
}

async function request<T>(endpoint: string, options?: RequestInit): Promise<T> {
  let response: Response
  try { response = await fetch('/api/' + endpoint, { credentials: 'same-origin', ...options }) }
  catch { throw new ApiError('无法连接到内网服务，请检查网络连接', 0) }
  const body = await response.json().catch(() => ({})) as { error?: string }
  if (!response.ok) throw new ApiError(body.error || `请求失败（${response.status}）`, response.status)
  return body as T
}

const json = (method: string, body: unknown): RequestInit => ({
  method, headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
})
const params = (values: Record<string, string | number>) => new URLSearchParams(
  Object.entries(values).map(([key, value]) => [key, String(value)]),
).toString()
const fileEndpoint = (operation: string, remoteId?: number | null) => remoteId == null ? operation : `remote/${remoteId}/${operation}`

export const api = {
  me: () => request<User>('me'),
  login: (username: string, password: string) => request<User>('login', json('POST', { username, password })),
  logout: () => request<{ ok: boolean }>('logout', { method: 'POST' }),
  requestPassword: (username: string) => request<{ message: string }>('password-requests', json('POST', { username })),
  passwordRequests: () => request<{ requests: PasswordRequest[] }>('password-requests'),
  reviewPassword: (id: number, decision: 'approve' | 'reject') => request<{ ok: boolean }>('password-requests/review', json('POST', { id, decision })),
  changePassword: (password: string) => request<{ ok: boolean }>('password', json('POST', { password })),
  serverInfo: () => request<ServerInfo>('server-info'),
  saveServerInfo: (name: string, storage_limit: number) => request<ServerInfo>('server-info', json('POST', { name, storage_limit })),
  list: (path: string, remoteId?: number | null) => request<FileList>(`${fileEndpoint('list', remoteId)}?${params({ path })}`),
  mkdir: (path: string, remoteId?: number | null) => request<{ ok: boolean }>(fileEndpoint('mkdir', remoteId), json('POST', { path })),
  move: (source: string, destination: string, remoteId?: number | null) => request<{ ok: boolean }>(fileEndpoint('move', remoteId), json('POST', { source, destination })),
  remove: (path: string, remoteId?: number | null) => request<{ ok: boolean }>(`${fileEndpoint('delete', remoteId)}?${params({ path })}`, { method: 'DELETE' }),
  users: () => request<{ users: User[] }>('users'),
  addUser: (username: string, password: string, role: User['role']) => request<{ ok: boolean }>('users', json('POST', { username, password, role })),
  removeUser: (id: number) => request<{ ok: boolean }>(`users?${params({ id })}`, { method: 'DELETE' }),
  grants: (userId: number) => request<{ grants: Grant[] }>(`grants?${params({ user_id: userId })}`),
  setGrant: (grant: { user_id: number; path: string } & Record<Permission, boolean>) => request<{ ok: boolean }>('grants', json('POST', grant)),
  removeGrant: (id: number) => request<{ ok: boolean }>(`grants?${params({ id })}`, { method: 'DELETE' }),
  remotes: () => request<{ outgoing: RemoteOutgoing[] }>('remotes'),
  addRemote: (url: string) => request<{ ok: boolean }>('remotes', json('POST', { url })),
  syncRemotes: () => request<{ ok: boolean; updated: number; errors: string[] }>('remotes/sync', json('POST', {})),
  incomingRemotes: () => request<{ incoming: RemoteIncoming[] }>('remotes/incoming'),
  reviewRemote: (id: string, decision: 'approve' | 'reject') => request<{ ok: boolean }>('remotes/incoming/review', json('POST', { id, decision })),
  setRemoteGrant: (grant: { id: string; path: string } & Record<Permission, boolean>) => request<{ ok: boolean }>('remotes/incoming/grants', json('POST', grant)),
  removeRemoteGrant: (id: number) => request<{ ok: boolean }>(`remotes/incoming/grants?${params({ id })}`, { method: 'DELETE' }),
}

export function downloadUrl(path: string, directory: boolean, remoteId?: number | null): string {
  return `/api/${fileEndpoint(directory ? 'folder.zip' : 'download', remoteId)}?${params({ path })}`
}

export function uploadFile(file: File, path: string, onProgress: (sent: number, total: number) => void, remoteId?: number | null): Promise<void> {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest()
    xhr.open('PUT', `/api/${fileEndpoint('upload', remoteId)}?${params({ path })}`)
    xhr.upload.onprogress = event => onProgress(event.loaded, event.total || file.size)
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) { resolve(); return }
      let message = `上传失败（${xhr.status}）`
      try { message = (JSON.parse(xhr.responseText) as { error?: string }).error || message } catch { /* response may be empty */ }
      reject(new ApiError(message, xhr.status))
    }
    xhr.onerror = () => reject(new ApiError('上传中断，请检查网络连接', 0))
    xhr.send(file)
  })
}
