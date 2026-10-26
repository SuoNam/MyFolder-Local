export const authStatus = { pending: '待审核', approved: '已通过', rejected: '已驳回', revoked: '已撤销', expired: '已过期' }
export const authTime = (value: number | null) => value ? new Date(value * 1000).toLocaleString('zh-CN') : '—'
export const browserLoginMessage = {
  approved: '此浏览器免密已开启，最长记住 90 天，关闭后重开可自动登录；点击“退出登录”会撤销免密。',
  pending: '这是旧版待审批记录；现在正确密码登录即可自动记住浏览器，无需再审批。',
  rejected: '旧浏览器凭据已失效；请使用正确密码登录，重新记住当前浏览器，无需再审批。',
  revoked: '此浏览器的旧凭据已撤销；使用正确密码登录可重新记住，旧凭据不能自动恢复。',
  expired: '此浏览器的登录凭据已过期；请使用正确密码登录，重新记住当前浏览器。',
  missing: '本次未记住浏览器，关闭后需要重新登录；个人电脑可在登录时勾选“记住此浏览器”。',
  https_required: '管理员已开启仅 HTTPS 免密登录，当前是临时登录；请从可信 HTTPS 入口领取或使用免密。',
}
