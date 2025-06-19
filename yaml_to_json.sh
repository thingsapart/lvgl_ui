#/bin/sh
if [ "$#" -ne 1 ]; then
  echo "Error: Incorrect number of arguments."
  echo "Usage: $0 <UI-SPEC json filename>"
  exit 1
fi

BASENAME=`basename "$1"`
OUTNAME=${BASENAME%.*}

echo "yq -o=json eval $1 > ${OUTNAME}.json"
yq -o=json eval "$1" > "${OUTNAME}.json"
