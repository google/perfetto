#!/bin/bash
set -e

echo ""
echo "Downloading latest copy of test data"
echo ""
LATEST_ZIP="$(cat tools/install-build-deps  | grep -o 'https://.*/perfetto/test-data-.*.zip')"
curl -o /tmp/latest-test-data.zip $LATEST_ZIP

echo ""
echo "Extracting test data to temp folder"
echo ""
rm -rf /tmp/latest-test-data 2>/dev/null
unzip /tmp/latest-test-data.zip -d /tmp/latest-test-data

echo ""
echo "Copying trace to temp folder"
echo ""
cp $1 /tmp/latest-test-data

echo ""
echo "Zipping file back up"
echo ""
NEW_TEST_DATA="test-data-$(date +%Y%m%d-%H%M%S).zip"
CWD="$(pwd)"
cd /tmp/latest-test-data
zip -r /tmp/$NEW_TEST_DATA *
cd $CWD

echo ""
echo "Uploading file to Google Cloud"
echo ""
gsutil cp /tmp/$NEW_TEST_DATA gs://perfetto/$NEW_TEST_DATA

echo ""
echo "Setting file to world readable"
echo ""
gsutil acl ch -u AllUsers:R gs://perfetto/$NEW_TEST_DATA

echo ""
echo "SHA1 of file $NEW_TEST_DATA is"
if which shasum; then
NEW_SHA=$(shasum /tmp/$NEW_TEST_DATA)  # Mac OS
else
NEW_SHA=$(sha1sum /tmp/$NEW_TEST_DATA)  # Linux
fi
echo $NEW_SHA

echo ""
echo "Cleaning up leftover files"
echo ""
rm -r /tmp/latest-test-data
rm /tmp/latest-test-data.zip
rm /tmp/$NEW_TEST_DATA

echo ""
echo "Updating tools/install-build-deps"
echo ""
OLD_URL="https://\(.*/perfetto\)/test-data-.*.zip"
NEW_URL="https://\1/$NEW_TEST_DATA"
OLD_SHA="\w*"
SED_MAGIC="s|'$OLD_URL',\n\(\s*\)'$OLD_SHA'|'$NEW_URL',\n\2'$NEW_SHA'|g"
sed -i '' -z -e "$SED_MAGIC" tools/install-build-deps

echo "All done!"
