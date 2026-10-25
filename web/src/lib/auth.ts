export const authStatus = { pending: '待审核', approved: '已通过', rejected: '已驳回', revoked: '已撤销', expired: '已过期' }
export const authTime = (value: number | null) => value ? new Date(value * 1000).toLocaleString('zh-CN') : '—'
