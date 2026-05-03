# 인코딩 샘플 코퍼스

수동/end-to-end 검증용 텍스트 파일. 단위 테스트는 `tests/test_main.c` 내부의
인라인 바이트 픽스처를 사용하므로 이 파일들은 hview 자체를 띄워 화면 비교를
하거나 인코딩 자동 판별 회귀를 점검할 때 쓴다.

## 파일

| 파일 | 인코딩 | BOM | 첫 4바이트 (hex) | 비고 |
|------|--------|----:|------------------|------|
| `empty.txt` | — | — | (없음) | 0바이트 경계 |
| `ascii.txt` | ASCII | — | `48 65 6C 6C` (`Hell`) | 멀티바이트 없음 |
| `utf8.txt` | UTF-8 | 없음 | `ED 95 9C EA` (한…) | 자동 판별 시 ENC_UTF8 |
| `utf8_bom.txt` | UTF-8 | 있음 | `EF BB BF ED` | 자동 판별 시 ENC_UTF8_BOM |
| `utf16_le.txt` | UTF-16 LE | 있음 | `FF FE 5C D5` (BOM + 한) | |
| `utf16_be.txt` | UTF-16 BE | 있음 | `FE FF D5 5C` (BOM + 한) | |
| `cp949.txt` | CP949 | — | `C7 D1 B1 DB` (한글) | EUC-KR 호환 |
| `johab.txt` | Johab | — | `D0 65 8B 69` (한 글) | KS X 1001-1992 부속서 3 |
| `long_line.txt` | UTF-8 | 없음 | `ED 95 9C ED` | 단일 줄 48KB — 줄 인덱스 경계 |

`한글` 동일 텍스트 — 인코딩만 다르게 변환:

```
한글 텍스트 인코딩 샘플
둘째 줄: 가나다라마바사
셋째 줄: ABC 123 가힣
```

## 재생성

`gen.py` 스타일을 다시 돌리려면 (Python 3.x):

```sh
python3 - <<'PY'
text = '한글 텍스트 인코딩 샘플\n둘째 줄: 가나다라마바사\n셋째 줄: ABC 123 가힣\n'
open('utf8.txt','wb').write(text.encode('utf-8'))
open('utf8_bom.txt','wb').write(b'\xef\xbb\xbf' + text.encode('utf-8'))
open('utf16_le.txt','wb').write(b'\xff\xfe' + text.encode('utf-16-le'))
open('utf16_be.txt','wb').write(b'\xfe\xff' + text.encode('utf-16-be'))
open('cp949.txt','wb').write(text.encode('cp949'))
open('johab.txt','wb').write(text.encode('johab'))
open('long_line.txt','wb').write((('한글' * 8000) + '\n').encode('utf-8'))
PY
```

## 사용 시나리오

1. **자동 판별 회귀:** hview를 빌드하고 각 파일을 드래그 → 타이틀바
   인코딩 표시가 위 표와 일치하는지 확인.
2. **수동 인코딩 메뉴:** 잘못 판별된 경우 메뉴 → 인코딩으로 강제 지정 후
   글자가 깨지지 않는지 확인.
3. **경계:** `empty.txt` (빈 파일), `long_line.txt` (단일 거대 줄)이
   크래시/멈춤 없이 열리는지.

## 향후 추가 (Phase 5 이후)

- `sjis.txt` — Shift-JIS 일본어 샘플
- `mixed_endings.txt` — \r\n, \n, \r 혼합
- `utf8_overlong.txt` — overlong 인코딩 거부 검증
- `64mb.txt` — 64MB 경계 (생성 후 .gitignore에 추가)
