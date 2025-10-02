#!/usr/bin/env bash
set -euo pipefail

# 사용자 정의 가능: 기본 corpus 루트 디렉토리 (기본=fuzz_test_0)
: "${CORPUS_ROOT:=test_all_fuzz}"

# fuzzer 실행 커맨드 커스터마이즈 가능
: "${RUN_FUZZER_CMD:=./run_fuzzer.sh}"
: "${MERGE_CMD:=./merge_coverage.sh}"
: "${REPORT_CANDIDATES:=report_coverage.sh reprot_coverage.sh}"  # 오타 대응

#ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
#cd "$ROOT_DIR"

echo "[INFO] CORPUS_ROOT=${CORPUS_ROOT}"

# tests 하위 1단계 디렉토리 중 exfat.img.tar.xz 보유한 것만
mapfile -t DIRS < <(find ./tests -mindepth 1 -maxdepth 1 -type d -print0 \
  | while IFS= read -r -d '' d; do
        if [ -f "$d/exfat.img.tar.xz" ]; then
            printf '%s\n' "$d"
        fi
    done | sort)

TOTAL=${#DIRS[@]}
if (( TOTAL == 0 )); then
  echo "[ERROR] exfat.img.tar.xz 를 가진 테스트 디렉토리가 없습니다."
  exit 1
fi

rm run_all_fuzz.log

# 선택적: 개별 실행 중 profraw 백업
mkdir -p coverage_raw

for i in "${!DIRS[@]}"; do
  DIR="${DIRS[$i]}"
  IDX=$((i+1))
  echo
  echo "=============================="
  echo "[$IDX/$TOTAL] 처리: $DIR"
  echo "=============================="

  # 1) corpus 디렉토리 재생성
  CORPUS_DIR="${CORPUS_ROOT}/corpus"
  rm -rf -- "${CORPUS_DIR}"
  mkdir -p "${CORPUS_DIR}"

  # 2) 이미지 풀기
  #   - tar 내부에 exfat.img 단일(혹은 몇 개) 있다고 가정
  #   - 여러 개일 경우 이름 충돌 방지를 위해 prefix 부여
  tar -xJf "${DIR}/exfat.img.tar.xz" -C "${CORPUS_DIR}"

  # 이름이 정확히 exfat.img 라면 고유화
  if [ -f "${CORPUS_DIR}/exfat.img" ]; then
      mv "${CORPUS_DIR}/exfat.img" "${CORPUS_DIR}/${IDX}_$(basename "${DIR}").img"
  fi

  # 3) 이전 fuzz_coverage.profraw 백업 (있으면)
  if [ -f fuzz_coverage.profraw ]; then
      mv fuzz_coverage.profraw "coverage_raw/pre_${IDX}.profraw"
  fi

  # 4) 퍼저 실행
  echo "[INFO] Running fuzzer: ${RUN_FUZZER_CMD}"
  ./test_fsck fuzz_test/corpus -runs=1000000 \
	  -max_len=5242880 -len_control=0 -prefer_small=0 \
	  -print_pcs=1 -print_final_stats=1 -ignore_crashes=1 -dump_coverage=1 \
	  | tee -a run_all_fuzz.log
  if [ ! $? ]; then
      echo "[ERROR] Fuzzer 실행 실패 (디렉토리: $DIR)"
      exit 1
  fi

  # 실행 후 profraw 백업
  if [ -f fuzz_coverage.profraw ]; then
      cp fuzz_coverage.profraw "coverage_raw/${IDX}_$(basename "$DIR").profraw"
  fi

  # 5) 커버리지 병합
  echo "[INFO] Merging coverage: ${MERGE_CMD}"
  if ! ${MERGE_CMD}; then
      echo "[ERROR] merge_coverage.sh 실패 (디렉토리: $DIR)"
      exit 1
  fi

  echo "[INFO] 완료: [$IDX/$TOTAL] $DIR"
done

echo
echo "[INFO] 모든 테스트 디렉토리 처리 완료. 최종 커버리지 리포트 생성 시도."

./report_coverage.sh

echo "[DONE] 전체 파이프라인 종료."
