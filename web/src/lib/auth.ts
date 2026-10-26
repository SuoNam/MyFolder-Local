export const authStatus = { pending: '待审核', approved: '已通过', rejected: '已驳回', revoked: '已撤销', expired: '已过期' }
export const authTime = (value: number | null) => value ? new Date(value * 1000).toLocaleString('zh-CN') : '—'
export const browserLoginMessage = {
  approved: '此浏览器免密已开启，直接关闭后重开可自动登录；点击“退出登录”会撤销免密。',
  pending: '此浏览器的免密申请仍待管理员批准，当前是临时登录；注册审批与浏览器免密审批不同。',
  rejected: '此浏览器的免密申请已被驳回，当前是临时登录；请联系管理员或重新申请。',
  revoked: '此浏览器的免密资格已撤销，当前是临时登录；如需免密，请重新申请并等待批准。',
  expired: '此浏览器的免密资格已过期，当前是临时登录；如需免密，请重新申请。',
  missing: '此浏览器尚未开启免密，关闭浏览器后需要重新登录；请前往“登录设备”申请免密并等待批准。',
  https_required: '管理员已开启仅 HTTPS 免密登录，当前是临时登录；请从可信 HTTPS 入口领取或使用免密。',
}
