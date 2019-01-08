#!/bin/bash
set -e

echo ""
echo "Downloading latest copy of test data"
echo ""
LATEST_ZIP="$(gsutil ls gs://perfetto | sort | grep test-data | tail -n 1)"
gsutil cp $LATEST_ZIP /tmp/latest-test-data.zip

echo ""
echo "Extracting test data to temp folder"
echo ""
unzip /tmp/latest-test-data.zip -d /tmp/latest-test-data

echo ""
echo "Copying trace to temp folder"
echo ""
cp $1 /tmp/latest-test-data

echo ""
echo "Zipping file back up"
echo ""
NEW_TEST_DATA="test-data-$(date +%Y%m%d).zip"
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
echo $(shasum /tmp/$NEW_TEST_DATA)  # Mac OS
else
echo $(sha1sum /tmp/$NEW_TEST_DATA)  # Linux
fi

echo ""
echo "Cleaning up leftover files"
echo ""
rm -r /tmp/latest-test-data
rm /tmp/latest-test-data.zip
rm /tmp/$NEW_TEST_DATA

echo ""
echo "All done! Please update tools/install-build-deps"
echo "with the new file name and sha1sum"
echo ""
