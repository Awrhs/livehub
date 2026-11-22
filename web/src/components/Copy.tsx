import { useState } from 'react'
import { useI18n } from '../i18n'

// 只读文本框 + 复制按钮；secret 为 true 时默认遮挡
export default function CopyField({ value, secret = false, testId }: { value: string; secret?: boolean; testId?: string }) {
  const { t } = useI18n()
  const [shown, setShown] = useState(!secret)
  const [copied, setCopied] = useState(false)
  const copy = async () => {
    try {
      await navigator.clipboard.writeText(value)
    } catch {
      // 非安全上下文（http）下 clipboard API 不可用：退回选中文本
      const ta = document.createElement('textarea')
      ta.value = value
      document.body.appendChild(ta)
      ta.select()
      document.execCommand('copy')
      ta.remove()
    }
    setCopied(true)
    window.setTimeout(() => setCopied(false), 1500)
  }
  return (
    <div className="secret">
      <input readOnly type={shown ? 'text' : 'password'} value={value} data-testid={testId} onFocus={(e) => e.currentTarget.select()} />
      {secret && (
        <button type="button" onClick={() => setShown((v) => !v)}>
          {shown ? t('common.hide') : t('common.show')}
        </button>
      )}
      <button type="button" onClick={copy}>
        {copied ? t('common.copied') : t('common.copy')}
      </button>
    </div>
  )
}
