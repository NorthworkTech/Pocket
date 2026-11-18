import { useEffect, useRef, useState } from 'react'
import { pairCodeFromScan } from '../lib/pairCode'

type Detector = { detect: (src: CanvasImageSource) => Promise<Array<{ rawValue: string }>> }

/**
 * Full-screen camera sheet that reads Pocket's pairing QR. Uses the browser's
 * BarcodeDetector where it exists (Chrome/Android) and the jsQR decoder elsewhere
 * (iPhone Safari). Calls `onCode` once with a valid 8-character pairing code.
 */
export function QrScanner({ onCode, onClose }: { onCode: (code: string) => void; onClose: () => void }) {
  const videoRef = useRef<HTMLVideoElement>(null)
  const [error, setError] = useState<string | null>(null)
  const [hint, setHint] = useState('Point the camera at the code on Pocket.')
  // Latest callback without restarting the camera when the parent re-renders.
  const onCodeRef = useRef(onCode)
  onCodeRef.current = onCode

  useEffect(() => {
    let stream: MediaStream | null = null
    let raf = 0
    let stopped = false
    let done = false
    const canvas = document.createElement('canvas')
    const ctx = canvas.getContext('2d', { willReadFrequently: true })
    let detector: Detector | null = null
    let jsqr: ((d: Uint8ClampedArray, w: number, h: number) => { data: string } | null) | null = null

    async function start() {
      if (!navigator.mediaDevices?.getUserMedia) {
        setError("This browser can't use the camera. Type the code instead.")
        return
      }
      try {
        stream = await navigator.mediaDevices.getUserMedia({
          video: { facingMode: { ideal: 'environment' } },
          audio: false,
        })
      } catch {
        setError('Camera access was blocked. Allow the camera for this site, or type the code.')
        return
      }
      if (stopped) {
        stream.getTracks().forEach((t) => t.stop())
        return
      }
      const BD = (window as unknown as { BarcodeDetector?: new (o: { formats: string[] }) => Detector })
        .BarcodeDetector
      if (BD) {
        try {
          detector = new BD({ formats: ['qr_code'] })
        } catch {
          detector = null
        }
      }
      if (!detector) {
        const mod = await import('jsqr')
        jsqr = mod.default as unknown as typeof jsqr
      }
      const v = videoRef.current
      if (!v) return
      v.srcObject = stream
      v.setAttribute('playsinline', 'true')
      await v.play().catch(() => undefined)
      tick()
    }

    function found(text: string) {
      const code = pairCodeFromScan(text)
      if (!code) {
        setHint("That isn't a Pocket pairing code. Scan the code on Pocket's Link screen.")
        return
      }
      if (done) return
      done = true
      if (navigator.vibrate) navigator.vibrate(30)
      onCodeRef.current(code)
    }

    async function tick() {
      if (stopped || done) return
      const v = videoRef.current
      if (v && v.readyState >= 2 && v.videoWidth > 0) {
        try {
          if (detector) {
            const hits = await detector.detect(v)
            if (hits[0]?.rawValue) found(hits[0].rawValue)
          } else if (jsqr && ctx) {
            const scale = Math.min(1, 640 / v.videoWidth)
            canvas.width = Math.round(v.videoWidth * scale)
            canvas.height = Math.round(v.videoHeight * scale)
            ctx.drawImage(v, 0, 0, canvas.width, canvas.height)
            const img = ctx.getImageData(0, 0, canvas.width, canvas.height)
            const hit = jsqr(img.data, img.width, img.height)
            if (hit?.data) found(hit.data)
          }
        } catch {
          /* keep scanning */
        }
      }
      raf = window.setTimeout(() => requestAnimationFrame(() => void tick()), 120) as unknown as number
    }

    void start()
    return () => {
      stopped = true
      window.clearTimeout(raf)
      stream?.getTracks().forEach((t) => t.stop())
    }
  }, [])

  return (
    <div className="qr-sheet" role="dialog" aria-modal="true" aria-label="Scan the code on Pocket">
      <div className="qr-frame">
        <video ref={videoRef} className="qr-video" muted playsInline />
        <div className="qr-target" aria-hidden="true" />
      </div>
      <p className="qr-hint" role="status">
        {error || hint}
      </p>
      <button type="button" className="btn btn-block qr-cancel" onClick={onClose}>
        {error ? 'Type the code instead' : 'Cancel'}
      </button>
    </div>
  )
}
